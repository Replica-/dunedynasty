// Post-processing shader, used when [graphics] gl_post_fx = true or --gl-post.
// Edit and restart the game; no rebuild needed (copy to dist/gfx/post.frag to
// try changes without re-running the build).
//
// Inputs:  al_tex (the finished frame), varying_texcoord (0..1),
//          u_resolution (window size in pixels), u_time (seconds)
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D al_tex;
uniform vec2 u_resolution;
uniform float u_time;
varying vec4 varying_color;
varying vec2 varying_texcoord;

void main() {
	vec4 c = texture2D(al_tex, varying_texcoord) * varying_color;

	// Faint scanlines and a vignette.
	float scan = 0.94 + 0.06 * sin(varying_texcoord.y * u_resolution.y * 3.14159);
	vec2 d = varying_texcoord - 0.5;
	float vig = 1.0 - dot(d, d) * 0.5;

	gl_FragColor = vec4(c.rgb * scan * vig, c.a);
}
