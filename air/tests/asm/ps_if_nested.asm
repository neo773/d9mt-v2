; nested conditionals
ps_3_0
dcl_color0 v0
if_gt v0.x, c0.x
if_lt v0.y, c1.x
mul r0, v0, c2
else
add r0, v0, c3
endif
else
mov r0, c4
endif
mov oC0, r0
