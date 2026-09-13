/**
 * @file edge-lighting-capi.h
 * @brief Flat C ABI over the EdgeLighting C++ renderer for FFI / C# interop.
 *
 * @section overview Overview
 *
 * This header is the only interop surface between native EdgeLighting and
 * managed callers (P/Invoke, ctypes, cgo, ...). Every symbol has C linkage,
 * uses only fixed-width scalar types + opaque pointers, and never leaks a
 * C++ exception - anything thrown at the boundary is caught and mapped to
 * @ref EL_ERROR_OUT_OF_MEMORY (on @c std::bad_alloc) or @ref EL_ERROR_INVALID_PARAMETER (on other C++ exceptions). Higher-level C++ features (RAII wrappers,
 * @c std::vector, @c glm) are hidden behind opaque handles.
 *
 * The C++ types the API mirrors live under @c lib/include/:
 * - @c EdgeLighting::EdgeLightingEffect (@c core/edge-lighting.h) - the
 *   orchestrator; one @c el_effect_handle_t wraps one instance plus its
 *   staging @c Config.
 * - @c EdgeLighting::Config (@c core/config.h) - geometry + per-renderer
 *   sub-configs; every @c el_effect_set_* mutates the effect's staging copy
 *   and every @c el_effect_get_* reads it back.
 * - @c EdgeLighting::Animation / @c FieldBoundAnimation
 *   (@c animation/animation.h, @c animation/field-bound-animation.h) -
 *   animations own their own play state; the flat animation factories below
 *   return one @c el_animation_handle_t per instance.
 * - @c EdgeLighting::Modulator (@c animation/modulator.h) - pure
 *   @c time -> float composables driving @c FieldBoundAnimation bindings.
 *
 * @section handles Opaque handles and ownership
 *
 * Three handle families:
 * - @c el_effect_handle_t - one per @ref el_effect_create call; destroy with
 *   @ref el_effect_destroy. Owns the GL resources allocated by
 *   @ref el_effect_init.
 * - @c el_animation_handle_t - one per @c el_animation_create* call; destroy
 *   with @ref el_animation_destroy. Attaching an animation to an effect does
 *   NOT transfer ownership - the caller must still destroy the handle. It is
 *   safe to destroy an attached animation; the effect drops its internal
 *   reference on the next @ref el_effect_detach_animation /
 *   @ref el_effect_detach_all_animations, so avoid leaking a still-attached
 *   handle across shutdown.
 * - @c el_modulator_handle_t - one per @c el_modulator_create* call; destroy
 *   with @ref el_modulator_destroy. Passing a modulator into a composite
 *   (@c Sequence / @c Multiplier / @c Adder / @c Remap) or into a
 *   @ref el_animation_from_modulator / @ref el_animation_add_field binding
 *   shares ownership - the composite retains a strong reference internally.
 *   The caller can (and should) destroy the outer @c el_modulator_handle_t
 *   once it is no longer needed to poke; the underlying modulator lives on
 *   until the last strong reference goes away.
 *
 * Handles are opaque pointer types - never dereference or pointer-arithmetic
 * them from the FFI side. Passing a null handle to any function returns
 * @ref EL_ERROR_INVALID_HANDLE; passing an already-destroyed handle is undefined
 * behaviour (mirror the same lifetime discipline you use for any handle).
 *
 * @section staging Staging config semantics
 *
 * The C++ effect exposes two configs internally - the "base" (what the last
 * @c SetConfig received) and the "active" (base + animation overlays, what
 * the renderers see). The C API exposes only the base as a *staging* config:
 *
 * - Every @c el_effect_set_* stashes a value in the effect's staging @c Config
 *   and calls @c SetConfig immediately so renderers pick it up next frame.
 * - Every @c el_effect_get_* reads that staging value back - not the
 *   animation-overlaid active value.
 * - @ref el_effect_capture re-syncs staging from the effect's authoritative
 *   base config; call it if you mutate the effect from C++ side channels
 *   (e.g. a preset applied inside the renderer that changes the base).
 *
 * @section errors Error model
 *
 * All fallible functions return an @ref el_result_e code. Value-returning
 * factories (@c el_effect_create / @c el_animation_create* /
 * @c el_modulator_create*) return @c NULL on failure. A caught exception at
 * the ABI boundary always maps to @ref EL_ERROR_OUT_OF_MEMORY (on @c std::bad_alloc) or @ref EL_ERROR_INVALID_PARAMETER (on other C++ exceptions); look at the native
 * log stream (see @c LOG_E) for the diagnostic message. Setters are also
 * idempotent - if the incoming value equals the staging value, no work is
 * done and @ref EL_SUCCESS is returned.
 *
 * @section threading Threading
 *
 * An effect has a DATA side (staging config, base config, clock, animations)
 * and a RENDER side (every renderer, every GL object). They meet at one
 * published config snapshot, so they can run on different threads.
 *
 * Every entry point below except @ref el_effect_render takes the handle's
 * internal lock, so calling them from any thread is safe. @ref el_effect_render
 * takes NO lock: it touches only the renderers and one atomic snapshot cell.
 * That is deliberate and load-bearing - a host thread dragging a slider or
 * walking a read loop can never stall a frame.
 *
 * | call group                                          | thread |
 * | --------------------------------------------------- | ------ |
 * | @c el_effect_create                                  | any |
 * | @c el_effect_init*                                   | the GL thread; first call before the data thread starts |
 * | @c el_effect_destroy                                 | the GL thread, AFTER the data thread has stopped |
 * | @c el_effect_set_* / @c _get_* / @c _read_* / @c _capture | any |
 * | @c el_effect_attach_animation / @c _detach* / @c _clock_* | any |
 * | @c el_effect_begin_batch / @c _end_batch              | any, closed by the thread that opened it |
 * | @c el_effect_update                                  | any, one thread at a time |
 * | @c el_effect_render                                  | the GL-owning thread, exclusively |
 * | @c el_animation_* / @c el_modulator_* factories      | any |
 *
 * Three things a host has to get right, none of which a lock can do for it:
 *
 * - **Destroy ordering.** @ref el_effect_destroy runs GL deletes and cannot
 *   lock a mutex it is about to destroy. Stop the data thread first. The
 *   library spawns no threads and cannot do this for you.
 * - **Callbacks run on the caller's thread.** An animation callback fires from
 *   inside @ref el_effect_update, on whichever thread called it, with the
 *   handle's lock held. Re-entering the effect from one is legal (the lock is
 *   recursive) and so is detaching the animation that fired it; BLOCKING in one
 *   stalls that thread. Marshal to your UI thread yourself.
 * - **A group of calls is not atomic by default.** Each call is individually
 *   safe, but an update can land between two of them - so setting colour stops
 *   in a loop can publish a half-updated gradient, and an
 *   @ref el_effect_read_count followed by indexed reads can straddle two
 *   frames. Every read re-validates its own index, so this is never a crash,
 *   only a mix of two frames. Wrap the group in
 *   @ref el_effect_begin_batch / @ref el_effect_end_batch when that matters.
 *
 * Animation handles are covered too, by ADOPTION: attaching hands the
 * animation the effect's lock, so every @c el_animation_* call on an attached
 * animation excludes @c el_effect_update. One mutex rather than two, which is
 * why an animation call can never deadlock against an effect call, and why a
 * host callback can call back into either. A DETACHED animation takes no lock
 * and belongs to whoever built it.
 *
 * Two things adoption does not cover:
 *
 * - **Attach and detach write the adopted lock**, and that write is not atomic.
 *   Do not attach or detach a handle while another thread is calling something
 *   else on that same handle. An animation handle also serves one effect at a
 *   time; detach before attaching it elsewhere.
 * - **Modulators have no lock.** Every modulator call but one is a factory or
 *   a pure evaluate; the exception, @c el_modulator_sequence_append, mutates
 *   and is construction-time only. There is no coherent lock to adopt, because
 *   one modulator can be bound into several animations across several effects.
 *   Build a sequence fully, then bind it.
 */
#ifndef _EDGE_LIGHTING_CAPI_H_
#define _EDGE_LIGHTING_CAPI_H_

#include "el-types.h"
#include "el-effect.h"
#include "el-animation.h"
#include "el-modulator.h"

/* Still-exported names scheduled for removal; see the checklist at the top of
 * that header. Included last so the replacements above are declared first. */
#include "el-deprecated.h"

#endif // _EDGE_LIGHTING_CAPI_H_
