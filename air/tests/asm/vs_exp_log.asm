; exp / log (full precision)
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
exp r0.x, c1.x
log r0.y, c2.x
rsq r0.z, c3.x
frc r0.w, c4.x
mov o0, v0
mov o1, r0
