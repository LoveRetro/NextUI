#version 100
precision mediump float;
uniform sampler2D Texture;
varying vec2 vTexCoord;

void main() {
    gl_FragColor = texture2D(Texture, vec2(vTexCoord.x, vTexCoord.y));
}
