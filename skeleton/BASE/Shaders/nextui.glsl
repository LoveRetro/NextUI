#version 100
precision mediump float;
uniform sampler2D uTex;
uniform vec2 texelSize;
varying vec2 vTexCoord;

void main() {
    vec2 offset = texelSize * 0.4;

    vec4 color       = texture2D(uTex, vTexCoord);
    vec4 colorRight  = texture2D(uTex, vTexCoord + vec2(offset.x, 0.0));
    vec4 colorDown   = texture2D(uTex, vTexCoord + vec2(0.0, offset.y));
    vec4 colorDiag   = texture2D(uTex, vTexCoord + offset);

    float weight = 0.5;
    vec4 smoothColor = mix(mix(color, colorRight, weight), mix(colorDown, colorDiag, weight), weight);

    gl_FragColor = smoothColor;
}
