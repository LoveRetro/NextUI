#version 100
precision mediump float;
uniform sampler2D uTex;
varying vec2 vTexCoord;

void main() {
    vec2 flippedCoord = vTexCoord;
    vec4 pixel = texture2D(uTex, flippedCoord);
    gl_FragColor = vec4(pixel.a, pixel.b, pixel.g, pixel.r);
}
