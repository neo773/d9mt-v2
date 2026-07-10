; loop aL with relative constant addressing c[aL + N]
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
defi i0, 4, 2, 1, 0
mov r0, v1
loop aL, i0
add r0, r0, c[aL + 3]
endloop
mov o0, v0
mov o1, r0
