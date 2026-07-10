; reciprocal + divide-style math
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
rcp r0.x, c1.x
rcp r0.y, c2.y
mul r1, v1, r0.x
mad r1, v1, r0.y, r1
mov o0, v0
mov o1, r1
