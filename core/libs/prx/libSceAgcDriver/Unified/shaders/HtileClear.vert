#version 450

layout(set = 0, binding = 1, std430) readonly buffer Result { uint state; } result;

layout(push_constant) uniform Push {
    float depth;
    uint bit;
} push;

void main() {
    if ((result.state & push.bit) != 0u) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, push.depth, 1.0);
}
