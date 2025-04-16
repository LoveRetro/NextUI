#version 100
precision mediump float;
uniform sampler2D uTex;
varying vec2 vTexCoord;

void main() {
    gl_FragColor = texture2D(uTex, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
}
