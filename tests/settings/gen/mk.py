# Helpers for tests/settings/gen/gen_all.py (the frame-rate scenarios, M6D D5; routes updated for the PC Settings
# categories, M7 feature 3; the frame rate on the Display page, M7 UX fix 3). The outputs are committed as plain files; regenerate with:
#   python3 -I tests/settings/gen/gen_all.py build/logs
import os
import os
T=os.environ.get('GEN_OUT', os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ''))
CONT=['500 tap Return','700 press Return','850 press Return','1000 press Down','1100 press Return',
      '1300 repeat 60 until gameplay press Return']
class S:
    def __init__(s, anchor='@gameplay+', t=300): s.a=anchor; s.t=t; s.l=[]
    def at(s, text, gap=30): s.l.append(f'{s.a}{s.t} {text}'); s.t+=gap
    def keys(s, key, n, gap=30, verb='press'):
        for _ in range(n): s.at(f'{verb} {key}', gap)
    def wait(s, n): s.t+=n
def to_fps_ingame(s, pad=False):
    p=(lambda k:f'pad press {k}') if pad else (lambda k:f'press {k}')
    s.at(p('back' if pad else 'Escape'),100)
    for _ in range(4): s.at(p('down' if pad else 'Down'),50)
    s.wait(50); s.at(p('a' if pad else 'Return'),150)
    for _ in range(8): s.at(p('down' if pad else 'Down'),30)
    s.wait(70); s.at(p('a' if pad else 'Return'),150)  # the categories (M7 feature 3)
    for _ in range(2): s.at(p('down' if pad else 'Down'),30)  # Display (M7 UX fix 3: the frame rate is on it)
    s.at(p('a' if pad else 'Return'),100)
    for _ in range(5): s.at(p('down' if pad else 'Down'),30)  # FPS, its sixth row (M7 UI polish)
    s.wait(40)
def leave_ingame(s, pad=False):  # the categories, Settings, the options menu, the game
    for _ in range(4): s.at('pad press b' if pad else 'press Escape',100)
    s.wait(100)
def write(name, header, scen, script_lines, extra=''):
    body='\n'.join(script_lines)
    if not any(' capture ' in l for l in script_lines): raise SystemExit(name + ': no capture')
    if '[format]' not in extra: extra = extra.rstrip('\n') + '\n\n[format]\nwidth = 1280\nheight = 720\n'
    open(T+name+'.toml','w').write(header.rstrip()+'\n\n[scenario]\n'+scen.rstrip()+'\nscript = """\n'+body+'\n"""\n'+extra.lstrip('\n'))
