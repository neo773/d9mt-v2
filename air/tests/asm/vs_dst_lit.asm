; dst + lit legacy lighting ops
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
dst r0, v1.y, v1.z
lit r1, v1
add r0, r0, r1
mov o0, v0
mov o1, r0
