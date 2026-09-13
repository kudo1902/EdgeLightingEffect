#ifndef _ADOPTED_LOCK_H_
#define _ADOPTED_LOCK_H_

#include <memory>
#include <mutex>

/// @file
/// The scope guard behind the C ABI's lock ADOPTION, which is how an animation
/// handle comes to share the lock of the effect it is attached to.
///
/// Lives in @c lib/capi rather than @c lib/include because adoption is an ABI
/// concept and nothing else: the C++ library has no animation handles, no
/// attachment bookkeeping outside @c AnimationManager, and nothing that needs a
/// lock it might not have. See @c el_animation_handle_impl for the mechanism
/// this serves and the two gaps it deliberately leaves open.

/// @brief Holds a lock that may not exist, for the rest of the enclosing scope.
///
/// An animation's mutex is null while it is detached, so the guard has to be a
/// no-op in that case. @c std::lock_guard cannot express that at all.
/// @c std::unique_lock CAN, in a single declaration and with no tag:
///
/// @code
///     std::unique_lock<std::recursive_mutex> lk =
///         m ? std::unique_lock<std::recursive_mutex>(*m)
///           : std::unique_lock<std::recursive_mutex>();
/// @endcode
///
/// That is a real alternative, the same size, and standard vocabulary. This
/// type exists for two things it does not give:
///
///   - **It keeps the mutex alive.** @c unique_lock stores a bare @c mutex*;
///     this stores a COPY of the shared_ptr, so the mutex cannot be freed under
///     the scope no matter who else lets go mid-call. Belt and braces rather
///     than a live bug - the handle's own reference already covers the
///     supported usage - but it makes the lifetime argument local instead of a
///     chain of reasoning about who else holds a reference.
///   - **It is reusable.** The @c unique_lock form only exists spelled out
///     inside a macro; anything else that needs to hold an adopted lock (a
///     batch scope, say) would have to repeat the ternary.
///
/// Move and copy are both deleted, unlike @c unique_lock, because neither has a
/// meaning here - a guard that can escape its scope is not a guard. Construct
/// it, let it die at the closing brace; @c LOCK_ANIMATION is the only intended
/// way to do that.
class AdoptedLock
{
public:
    explicit AdoptedLock(std::shared_ptr<std::recursive_mutex> mutex)
        : mMutex(std::move(mutex))
    {
        if (mMutex)
        {
            mMutex->lock();
        }
    }

    ~AdoptedLock()
    {
        if (mMutex)
        {
            mMutex->unlock();
        }
    }

    AdoptedLock(const AdoptedLock &) = delete;
    AdoptedLock &operator=(const AdoptedLock &) = delete;

private:
    std::shared_ptr<std::recursive_mutex> mMutex;
};

#endif // _ADOPTED_LOCK_H_
