#version 100
precision mediump float;
uniform sampler2D Texture;
varying vec2 vTexCoord;

void main() {
    vec2 flippedCoord = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec4 pixel = texture2D(Texture, flippedCoord);
    gl_FragColor = vec4(pixel.a, pixel.b, pixel.g, pixel.r);
}
