; vFace builtin: front-facing sign selects the color
ps_3_0
dcl vFace
def c30, 1.0, 0.1, 0.1, 1.0
def c31, 0.1, 0.1, 1.0, 1.0
cmp oC0, vFace, c30, c31
