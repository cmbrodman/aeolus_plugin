#!/usr/bin/env python3
"""Build diagnostic objects over validated HEALTH 2 objects; class layouts unchanged."""
import argparse,json,subprocess,concurrent.futures,os,shlex
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--plan',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
m=json.loads(a.plan.read_text());s=a.source.resolve();out=a.output.resolve();out.mkdir(exist_ok=False)
base=m['compile'][0];old=Path(base[base.index('-c')+1]).parent.parent
changed=['PluginProcessor.cpp','aeolus/engine.cpp','aeolus/division.cpp','aeolus/voice.cpp','aeolus/paniccontrol.cpp']
objects={};commands=[]
for cmd in m['compile']:
 src=Path(cmd[cmd.index('-c')+1]);obj=cmd[cmd.index('-o')+1]
 if not any(str(src).endswith('/Source/'+x) for x in changed) and src.name!='AudioTests.cpp':continue
 new=[x.replace(str(old),str(s)) for x in cmd];new[new.index('-o')+1]=str(out/Path(obj).name)
 objects[obj]=new[new.index('-o')+1];commands.append(new)
for module in ['juce_audio_devices','juce_audio_utils']:
 cmd=[x.replace(str(old),str(s)) for x in base];cmd[cmd.index('-c')+1]=str(s/'diagnostic-juce'/f'{module}.cpp');cmd[cmd.index('-o')+1]=str(out/f'{module}.o');commands.append(cmd)
links=[]
for original in m.get('links',[m.get('link')]):
 cmd=[objects.get(x,x) for x in original];cmd[cmd.index('-o')+1]=str(out/Path(original[original.index('-o')+1]).name)
 # Precede the shared archive so its uninstrumented module objects are not pulled.
 pos=next(i for i,x in enumerate(cmd) if x.endswith('SharedCode.a'))
 cmd[pos:pos]=[str(out/f'{module}.o') for module in ['juce_audio_devices','juce_audio_utils']]
 links.append(cmd)
(out/'build-commands.txt').write_text('\n'.join(shlex.join(x) for x in commands+links)+'\n')
def run(cmd):
 print('Compiling '+Path(cmd[cmd.index('-c')+1]).name,flush=True);subprocess.run(cmd,cwd=m['cwd'],check=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(run,commands))
for cmd in links:print('Linking '+cmd[cmd.index('-o')+1],flush=True);subprocess.run(cmd,cwd=m['cwd'],check=True)
e=os.environ.copy();e['AEOLUS_TEST_DATA_DIRECTORY']=str(out/'test-data')
if 'links' in m:
 for proc in Path('/proc').iterdir():
  try:
   if (proc/'comm').read_text().strip()!='Xwayland':continue
   args=(proc/'cmdline').read_bytes().decode().split('\0');auth=args[args.index('-auth')+1]
   if Path(auth).is_file():e['DISPLAY']=args[1];e['XAUTHORITY']=auth;break
  except (OSError,ValueError):pass
 test=[str(out/'aeolus_tests')]
else:test=['xvfb-run','-a',str(out/'aeolus_tests')]
subprocess.run(test,cwd=m['cwd'],env=e,check=True)
print('MIDI DIAGNOSTIC BUILD AND AUDIO REGRESSIONS PASSED',flush=True)
