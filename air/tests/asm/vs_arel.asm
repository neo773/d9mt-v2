; mova + relative constant addressing
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
mova a0.x, c1.x
mov r0, c[a0.x + 2]
mad r0, r0, c5.x, v1
mov o0, v0
mov o1, r0
