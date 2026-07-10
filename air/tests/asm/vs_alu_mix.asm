; ALU mix: add/sub/mul/mad/min/max
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
add r0, v1, c0
sub r1, r0, c1
mul r2, r1, c2
mad r3, r2, c3, v1
min r4, r3, c4
max r5, r4, c5
mov o0, v0
mov o1, r5
