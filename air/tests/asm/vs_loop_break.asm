; rep with conditional break
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
defi i0, 8, 0, 0, 0
mov r0, v1
mov r1.x, c9.x
rep i0
add r0, r0, c7
add r1.x, r1.x, c8.x
break_gt r1.x, c10.x
endrep
mov o0, v0
mov o1, r0
