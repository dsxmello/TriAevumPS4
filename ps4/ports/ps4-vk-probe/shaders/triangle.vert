#version 450
// Spinning triangle from gl_VertexIndex (no vertex buffers).
layout(push_constant) uniform Push {
  float angle;
  float aspect;  // height / width
} pc;
layout(location = 0) out vec3 color;

void main() {
  const vec2 corners[3] = vec2[](vec2(0.0, -0.7), vec2(0.6, 0.45), vec2(-0.6, 0.45));
  const vec3 colors[3] = vec3[](vec3(1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1), vec3(0.1, 0.3, 1.0));
  vec2 p = corners[gl_VertexIndex];
  float s = sin(pc.angle), c = cos(pc.angle);
  gl_Position = vec4((p.x * c - p.y * s) * pc.aspect, p.x * s + p.y * c, 0.0, 1.0);
  color = colors[gl_VertexIndex];
}
