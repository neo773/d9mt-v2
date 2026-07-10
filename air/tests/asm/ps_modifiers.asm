; source modifiers: abs instruction, negate modifier
ps_3_0
dcl_color0 v0
abs r0, v0
add r1, c2, -r0
mul r2, r1, -v0
abs r2, r2
mov oC0, r2
