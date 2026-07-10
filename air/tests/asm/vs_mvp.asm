; dp4 matrix transform (c0-c3) + color modulate by c4
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
dp4 r0.x, v0, c0
dp4 r0.y, v0, c1
dp4 r0.z, v0, c2
dp4 r0.w, v0, c3
mov o0, r0
mul o1, v1, c4
