precision highp float;

in vec3  vColor;
in float vLife;

out vec4 fragColor;

void main() {
    // gl_PointCoord ∈ [0, 1] across the point sprite. Recentre to [-0.5, 0.5]
    // and use radial distance to shape a soft round spark.
    vec2  uv = gl_PointCoord - vec2(0.5);
    float d  = length(uv) * 2.0;         // 0 at centre, 1 at edge.

    // Soft edge: full brightness inside 60% radius, smoothly fades to 0 at rim.
    float shape = 1.0 - smoothstep(0.6, 1.0, d);

    // Life fade in on the last 30% of the particle's life so newborns pop in
    // softly rather than appearing at full size.
    float birthFade = smoothstep(0.0, 0.15, vLife);
    float alpha     = shape * vLife * birthFade;

    if (alpha <= 0.0) { discard; }

    // Premultiplied so the renderer can composite with GL_ONE / one-minus-src-alpha
    // just like the neon shader does.
    fragColor = vec4(vColor * alpha, alpha);
}
