; nested rep loops
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
defi i0, 3, 0, 0, 0
defi i1, 2, 0, 0, 0
mov r0, v1
rep i0
rep i1
add r0, r0, c7
endrep
mul r0, r0, c8.x
endrep
mov o0, v0
mov o1, r0
