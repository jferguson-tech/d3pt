// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
#version 450

layout(set = 0, binding = 0) uniform sampler2D overlay;
layout(set = 0, binding = 1) uniform sampler2D traced;

layout(push_constant) uniform Push
{
	vec4	view;		// x, y, width, height in pixels
	vec2	screen;		// width, height in pixels
	float	has_view;	// there is a traced picture to show in the view
} pc;

layout(location = 0) out vec4 color;

void main()
{
	vec2 p = gl_FragCoord.xy;
	vec3 bg = vec3(0.0);

	// the traced picture fills the view, stretched if it was made smaller
	if (pc.has_view > 0.5 && p.x >= pc.view.x && p.y >= pc.view.y &&
		p.x < pc.view.x + pc.view.z && p.y < pc.view.y + pc.view.w)
		bg = texture(traced, (p - pc.view.xy) / max(pc.view.zw, vec2(1.0))).rgb;

	vec4 ov = texture(overlay, p / pc.screen);	// premultiplied
	color = vec4(ov.rgb + bg * (1.0 - ov.a), 1.0);
}
