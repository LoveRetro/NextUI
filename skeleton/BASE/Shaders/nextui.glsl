#version 100
precision mediump float;
uniform sampler2D Texture;
uniform vec2 texelSize;
varying vec2 vTexCoord;

void main() {
    vec2 offset = texelSize * 0.4;

    vec4 color       = texture2D(Texture, vTexCoord);
    vec4 colorRight  = texture2D(Texture, vTexCoord + vec2(offset.x, 0.0));
    vec4 colorDown   = texture2D(Texture, vTexCoord + vec2(0.0, offset.y));
    vec4 colorDiag   = texture2D(Texture, vTexCoord + offset);

    float weight = 0.5;
    vec4 smoothColor = mix(mix(color, colorRight, weight), mix(colorDown, colorDiag, weight), weight);

    gl_FragColor = smoothColor;
}
