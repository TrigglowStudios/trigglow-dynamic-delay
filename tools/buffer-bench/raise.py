import re,sys
for n in sys.argv[1:]:
    prev=None; flat=0; maxflat=(0,0); resumed=None
    for l in open(f'logs/{n}.log'):
        m=re.match(r't=\s*(\d+) .*delay=(\d+).*behind=([\d.]+)s .*bytes=(\d+)',l)
        if not m: continue
        t,d,b,by=int(m[1]),int(m[2]),float(m[3]),int(m[4])
        if prev is not None and by==prev and 'ended=0' in l or (prev is not None and by==prev and d>0 and t>=60):
            flat+=1
        else: flat=0
        if flat>maxflat[0] and t>20: maxflat=(flat,t)
        if t>660 and resumed is None and d==300 and b>=299: resumed=t
        prev=by
    print(n,'raise reached 300s behind at t=',resumed,'(after',None if resumed is None else resumed-660,'s); longest no-bytes stretch after t=20:',maxflat)
