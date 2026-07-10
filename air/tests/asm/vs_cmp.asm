; comparison ops: slt / sge producing 0/1 masks
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
slt r0, v1, c2
sge r1, v1, c3
mad r2, r0, c4, r1
min r2, r2, c5.w
mov o0, v0
mov o1, r2
