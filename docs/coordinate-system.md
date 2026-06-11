# Coordinate System

## Spaces

| Space | Origin | +X | +Y | Usage |
|---|---|---|---|---|
| **App** | viewport top-left | right | down | `Config::position` — rectangle top-left corner |
| **Local** | rectangle center | right | up | SDF, `GeometryUtils::GetPointOnRectangle`, particle spawn positions |
| **OpenGL** | viewport bottom-left | right | up | MVP model `translate()` (Y flipped from app space) |
| **NDC** | screen center | right | up | `gl_Position` output |

## Conversions

```
App → OpenGL:  ogl_y = viewportH - app_y
Local → App:  app = (position.x + width/2, position.y + height/2) + local
App → NDC:    ndc_x = (app_x - viewportW/2) / (viewportW/2)
              ndc_y = (viewportH/2 - app_y) / (viewportH/2)
```

## Segment Renderer (MVP)

```
center_ogl = (config.position.x + halfW, viewportH - config.position.y - halfH)
model = translate(center_ogl)
proj  = ortho(0, viewportW, 0, viewportH)
mvp   = proj * model
```

Quad vertices are in local pixel coords covering `±(halfSize + glowWidth + 5)`.

## Particles

- Spawn positions from `GetPointOnRectangle()` are in **Local** coords (relative to rect center, +Y up).
- `uOffset` uniform converts Local → NDC:

```
uOffset = (position.x + halfW - viewportW/2, viewportH/2 - position.y - halfH)
```

- Particle vertex shader: `worldPos = aPos + uOffset; ndcPos = worldPos / (uResolution * 0.5)`
