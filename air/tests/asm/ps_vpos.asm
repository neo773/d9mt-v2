; vPos builtin: pixel-coordinate gradient
ps_3_0
dcl vPos.xy
def c30, 0.0078125, 0.0078125, 0.0, 0.0
def c31, 0.0, 0.0, 0.5, 1.0
mad oC0, vPos.xyxx, c30, c31
