"""Render the firmware-matched wiring diagram as SVG and PNG."""
from pathlib import Path
from html import escape
import argparse
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--lang', choices=('hu', 'en'), default='hu')
parser.add_argument('--font-regular', help='Optional path to a TrueType regular font')
parser.add_argument('--font-bold', help='Optional path to a TrueType bold font')
args = parser.parse_args()
LANGUAGE = args.lang
ENGLISH = {
    'GAS READER • végleges bekötés': 'GAS READER • Final wiring diagram',
    'Seeed XIAO ESP32-C6  |  GPIO-lábkiosztás  |  antenna nélkül':
        'Seeed XIAO ESP32-C6  |  Firmware GPIO assignments  |  Antenna not shown',
    'Jelképes elrendezés': 'Logical symbol only',
    'Nem fizikai lábsorrend.': 'Not the physical pin order.',
    'D3 / GPIO21: szabad': 'D3 / GPIO21: unused',
    '1  Akkufeszültség-osztó': '1  Battery voltage divider',
    'R2 és C1 párhuzamosan kerül GND-re.': 'R2 and C1 connect in parallel to GND.',
    '2  Geckollect reed szenzor': '2  Geckollect reed sensor',
    'Két vezeték, polaritás nélkül.': 'Two wires; either polarity.',
    'NO = alaphelyzetben nyitott érintkező.': 'NO = normally open contact.',
    '3  Nyomógomb': '3  Push button',
    'NO gomb': 'NO button',
    'Belső felhúzás a 3V3-ra:': 'Internal pull-up to 3V3:',
    'a firmware kapcsolja be.': 'enabled by the firmware.',
    'A gomb két kapcsolt pontját használd.': 'Use the two terminals that close when pressed.',
    '4  Külső zöld LED': '4  External green LED',
    'anód': 'anode',
    'katód': 'cathode',
    'zöld': 'green',
    'Anód: hosszabb láb. Katód: rövidebb láb,': 'Anode: long lead. Cathode: short lead,',
    'a LED házának lapos oldalán.': 'next to the flat side of the LED body.',
    'Védett, egycellás 18650': 'Protected single-cell 18650',
    '3,7 V névleges • 4,2 V teljesen feltöltve': '3.7 V nominal • 4.2 V fully charged',
    'Az akku a hátoldali BAT+ / BAT− pontokra kerül.': 'Connect battery to rear BAT+ / BAT− pads.',
    'Az osztó BAT+ pontja ugyanaz az akku + pont.': 'Divider BAT+ is the same battery positive node.',
    'Minden GND jel közös: XIAO GND = BAT−.  A D0, D1, D2 és D4 jelek különállók.':
        'All GND symbols share one net: XIAO GND = BAT−. D0, D1, D2 and D4 are separate signals.',
}
W, H = 1500, 1100
SCALE = 2
im = Image.new('RGB', (W*SCALE, H*SCALE), 'white')
d = ImageDraw.Draw(im)
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', '<rect width="100%" height="100%" fill="white"/>']
INK = '#17324d'
BLUE = '#1565c0'
GREEN = '#17834b'
GRAY = '#526477'
RED = '#a93131'

def load_font(size, bold):
    override = args.font_bold if bold else args.font_regular
    candidates = [override] if override else [
        '/System/Library/Fonts/Supplemental/Arial Bold.ttf' if bold else '/System/Library/Fonts/Supplemental/Arial.ttf',
        'DejaVuSans-Bold.ttf' if bold else 'DejaVuSans.ttf',
    ]
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size*SCALE)
        except OSError:
            continue
    raise RuntimeError('No TrueType font found; use --font-regular and --font-bold.')

def line(points, color=INK, width=3):
    d.line([(int(x*SCALE),int(y*SCALE)) for x,y in points], fill=color, width=width*SCALE)
    svg.append(f'<polyline points="{" ".join(f"{x},{y}" for x,y in points)}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linejoin="round"/>')

def rect(x,y,w,h,fill='white',stroke=INK,width=2):
    d.rectangle((x*SCALE,y*SCALE,(x+w)*SCALE,(y+h)*SCALE),fill=fill,outline=stroke,width=width*SCALE)
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}" stroke-width="{width}"/>')

def text(x,y,s,size=22,color=INK,bold=False,anchor='start'):
    if LANGUAGE == 'en':
        s = ENGLISH.get(s, s)
    font=load_font(size, bold)
    d.text((x*SCALE,y*SCALE),s,font=font,fill=color,anchor={'start':'lt','middle':'mt','end':'rt'}[anchor])
    svg.append(f'<text x="{x}" y="{y}" dominant-baseline="text-before-edge" text-anchor="{anchor}" font-family="Arial, sans-serif" font-size="{size}" font-weight="{700 if bold else 400}" fill="{color}">{escape(s)}</text>')

def dot(x,y,color=INK):
    r=5;d.ellipse(((x-r)*SCALE,(y-r)*SCALE,(x+r)*SCALE,(y+r)*SCALE),fill=color)
    svg.append(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"/>')

def polygon(points,fill='white',stroke=INK):
    d.polygon([(x*SCALE,y*SCALE) for x,y in points],fill=fill)
    line(points+[points[0]],stroke)
    svg.append(f'<polygon points="{" ".join(f"{x},{y}" for x,y in points)}" fill="{fill}" stroke="{stroke}" stroke-width="3"/>')

def ground(x,y):
    line([(x,y),(x,y+12)])
    for n,w in enumerate([22,14,6]):line([(x-w,y+12+n*7),(x+w,y+12+n*7)])
    text(x,y+39,'GND',19,GRAY,anchor='middle')

def resistor_v(x,y1,y2):
    line([(x,y1),(x,y1+12)]);rect(x-12,y1+12,24,y2-y1-24);line([(x,y2-12),(x,y2)])

def switch_v(x,y1,y2):
    line([(x,y1),(x,y1+12)]);dot(x,y1+12)
    line([(x,y2),(x,y2-12),(x-23,y1+23)]);dot(x,y2-12)

text(55,30,'GAS READER • végleges bekötés',34,bold=True)
text(55,79,'Seeed XIAO ESP32-C6  |  GPIO-lábkiosztás  |  antenna nélkül',21,GRAY)
line([(55,117),(1445,117)],'#dce4eb',2)

# Logical board symbol; signal pins are deliberately not joined together.
rect(630,170,300,705,'#f1f6fc',BLUE,3)
text(780,204,'SEEED XIAO',26,BLUE,True,'middle')
text(780,242,'ESP32-C6',30,BLUE,True,'middle')
for y,label,side in [(320,'D0 / GPIO0','left'),(555,'3V3','left'),(660,'D1 / GPIO1','left'),(320,'D2 / GPIO2','right'),(660,'D4 / GPIO22','right')]:
    text(648 if side=='left' else 912,y-30,label,20,BLUE,True,'start' if side=='left' else 'end')
    dot(630 if side=='left' else 930,y,BLUE)
text(780,418,'Jelképes elrendezés',20,GRAY,anchor='middle')
text(780,448,'Nem fizikai lábsorrend.',18,GRAY,anchor='middle')
text(780,753,'D3 / GPIO21: szabad',19,GRAY,anchor='middle')
text(720,828,'BAT+',20,RED,True,'middle')
text(840,828,'BAT−',20,INK,True,'middle')
dot(720,875,RED);dot(840,875)

# Battery divider.
text(65,139,'1  Akkufeszültség-osztó',25,bold=True)
text(410,181,'BAT+',22,RED,True,'middle')
line([(410,207),(410,215)],RED)
resistor_v(410,215,285)
text(448,229,'R1',21,bold=True);text(448,257,'1 MΩ • 1%',21)
line([(410,285),(410,345)]);dot(410,320)
line([(410,320),(630,320)],BLUE)
resistor_v(410,345,415)
text(448,353,'R2',21,bold=True);text(448,381,'330 kΩ • 1%',21)
line([(410,415),(410,437)]);ground(410,437)
line([(410,320),(230,320),(230,370)])
line([(206,370),(254,370)]);line([(206,382),(254,382)])
line([(230,382),(230,437)]);ground(230,437)
text(75,354,'C1',21,bold=True);text(75,382,'100 nF',21)
text(65,500,'R2 és C1 párhuzamosan kerül GND-re.',18,GRAY)

# Reed sensor with external pull-up, two-wire normally open dry contact.
text(65,526,'2  Geckollect reed szenzor',25,bold=True)
line([(630,555),(410,555),(410,580)],GREEN)
text(363,557,'3V3',19,GREEN,True,'end')
resistor_v(410,580,640)
text(448,585,'R3',21,bold=True);text(448,613,'100 kΩ',21)
line([(410,640),(410,700)]);dot(410,660)
line([(410,660),(630,660)],BLUE)
switch_v(410,700,765)
text(70,704,'REED • NO',21,bold=True)
text(70,735,'Két vezeték, polaritás nélkül.',18,GRAY)
line([(410,765),(410,782)]);ground(410,782)
text(65,845,'NO = alaphelyzetben nyitott érintkező.',18,GRAY)

# Button uses retained internal pull-up, no external resistor needed.
text(990,139,'3  Nyomógomb',25,bold=True)
line([(930,320),(1170,320),(1170,352)],BLUE)
switch_v(1170,352,417)
line([(1170,417),(1170,440)]);ground(1170,440)
line([(1127,375),(1142,375)],GRAY,2)
line([(1127,366),(1127,384)],GRAY,2)
text(1210,355,'SW1',21,bold=True)
text(1210,385,'NO gomb',21)
text(990,220,'Belső felhúzás a 3V3-ra:',20,GRAY)
text(990,249,'a firmware kapcsolja be.',20,GRAY)
text(990,500,'A gomb két kapcsolt pontját használd.',18,GRAY)

# Active-high green LED, resistor and diode in series.
text(990,526,'4  Külső zöld LED',25,bold=True)
line([(930,660),(1000,660)],BLUE)
rect(1000,648,80,24)
line([(1080,660),(1150,660)])
text(1040,583,'R4',21,bold=True,anchor='middle')
text(1040,611,'1 kΩ',21,anchor='middle')
polygon([(1150,642),(1150,678),(1182,660)],'#e4f4e8')
line([(1185,639),(1185,681)])
line([(1185,660),(1370,660),(1370,695)]);ground(1370,695)
text(1143,702,'A',20,GREEN,True,'middle')
text(1192,702,'K',20,GREEN,True,'middle')
text(1143,733,'anód',17,GRAY,anchor='middle')
text(1206,758,'katód',17,GRAY,anchor='middle')
for x,y in [(1168,630),(1184,641)]:
    line([(x,y),(x+28,y-28)],GREEN,2)
    line([(x+14,y-25),(x+28,y-28),(x+25,y-14)],GREEN,2)
text(1260,582,'LED1',21,GREEN,True)
text(1260,611,'zöld',21,GREEN)
text(990,802,'Anód: hosszabb láb. Katód: rövidebb láb,',18,GRAY)
text(990,830,'a LED házának lapos oldalán.',18,GRAY)

# Protected single-cell battery and dedicated board battery pads.
rect(65,909,505,106,'#fff8f4','#e8cfc1',2)
text(85,926,'Védett, egycellás 18650',23,bold=True)
text(85,961,'3,7 V névleges • 4,2 V teljesen feltöltve',19,GRAY)
text(538,917,'+',25,RED,True)
text(538,972,'−',25,INK,True)
line([(570,933),(720,933),(720,875)],RED)
text(625,903,'BAT+',18,RED,True,'middle')
line([(570,990),(840,990),(840,875)])
text(880,935,'Az akku a hátoldali BAT+ / BAT− pontokra kerül.',21)
text(880,970,'Az osztó BAT+ pontja ugyanaz az akku + pont.',19,GRAY)

line([(55,1037),(1445,1037)],'#dce4eb',2)
text(55,1054,'Minden GND jel közös: XIAO GND = BAT−.  A D0, D1, D2 és D4 jelek különállók.',21,bold=True)
svg.append('</svg>')
stem = 'wiring-en' if LANGUAGE == 'en' else 'wiring'
(ROOT/f'docs/{stem}.svg').write_text('\n'.join(svg)+'\n')
im.save(ROOT/f'docs/{stem}.png')
print(f'Created docs/{stem}.svg and docs/{stem}.png')
