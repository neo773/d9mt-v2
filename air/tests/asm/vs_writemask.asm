; write-mask stress: partial destination writes
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
mov r0, c6
mov r0.x, v1.y
add r0.yz, r0, v1
mul r0.w, r0.x, c7.x
mov o0, v0
mov o1, r0
