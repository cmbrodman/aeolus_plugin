#!/usr/bin/env python3
"""Read-only MIDI diagnostic observer. Writes only to a private RAM directory."""
import argparse,json,os,select,socket,time,secrets,shutil
from pathlib import Path
STAGES=['device','enqueue','dequeue','processor','engine']
EXTRAS=['collector_reset','age_pruned','window_pruned','engine_off','division_off','release_matched','voice_release','pool_trigger','pool_failure','pool_return','queue_overflow','age_pruned_off','window_pruned_off','reset_pruned_off']
p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--executable',type=Path,required=True);p.add_argument('--socket',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--duration',type=float,default=0);p.add_argument('--archive',type=Path);a=p.parse_args()
assert a.pid>1 and os.stat('/proc/'+str(a.pid)).st_uid==os.getuid()
assert os.readlink('/proc/'+str(a.pid)+'/exe')==str(a.executable.resolve())
assert str(a.output.resolve()).startswith('/dev/shm/')
a.output.mkdir(mode=0o700,parents=False,exist_ok=False)
fd=os.pidfd_open(a.pid);sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind('\0aeolus-midi-observer-'+secrets.token_hex(8));sock.settimeout(.15)
def request(cmd):
 try:
  sock.sendto(cmd.encode(),a.socket)
  deadline=time.monotonic()+.15
  while time.monotonic()<deadline:
   reply=sock.recv(1024).decode().split()
   if not reply or reply[0]!=cmd.split()[0]:continue
   if reply[0]=='NOTE' and reply[2:4]!=cmd.split()[1:]:continue
   return reply
  return []
 except (OSError,UnicodeError):return []
start=time.monotonic();next_keys=start
try:
 with (a.output/'midi.jsonl').open('x') as out:
  while (a.duration<=0 or time.monotonic()-start<a.duration) and not select.select([fd],[],[],0)[0]:
   row={'time_ns':time.monotonic_ns()}
   v=request('DIAG')
   if len(v)==2+2*len(STAGES)+len(EXTRAS) and v[:2]==['DIAG','1']:
    ns=list(map(int,v[2:]));row['stages']={s:{'on':ns[2*i],'off':ns[2*i+1]} for i,s in enumerate(STAGES)};row['extra']=dict(zip(EXTRAS,ns[10:]))
   else:row['diagnostic_error']=v
   row['health']=request('HEALTH');row['status']=request('STATUS')
   out.write(json.dumps(row)+'\n');out.flush()
   if time.monotonic()>=next_keys:
    # Rotate all channels; channel 1 (normal GX61 channel) every sweep.
    channel=1+(int((time.monotonic()-start)//5)%16)
    for ch in sorted({1,channel}):
     for note in range(128):
      v=request(f'NOTE {ch} {note}')
      if not v:break
      if len(v)==14 and any(int(x) for x in v[4:]):
       out.write(json.dumps({'time_ns':time.monotonic_ns(),'key':v})+'\n')
    out.flush();next_keys=time.monotonic()+5
   time.sleep(.25)
finally:
 exited=bool(select.select([fd],[],[],0)[0])
 sock.close();os.close(fd)
 # Persist only after the target has exited, keeping disk writes away from play.
 if a.archive and exited:
  a.archive.mkdir(parents=True,exist_ok=False)
  for file in a.output.iterdir():
   if file.is_file():shutil.copy2(file,a.archive/file.name)
