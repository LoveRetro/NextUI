#version 100
attribute vec2 VertexCoord;
attribute vec2 TexCoord;
varying vec2 vTexCoord;

void main() {
    vTexCoord = TexCoord;
    gl_Position = vec4(VertexCoord, 0.0, 1.0);
}