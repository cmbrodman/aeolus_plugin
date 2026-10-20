#!/usr/bin/env python3
"""Silent real-device checks for the isolated panic build; no MIDI is sent."""
from pathlib import Path
import argparse,csv,json,os,subprocess,time,signal,ctypes as c
from aeolus_panic_client import PanicRecovery

def close_window(pid, env):
    placement={"pid":pid}
    # Send WM_DELETE_WINDOW only to this test process's named main window.
    x=c.CDLL('libX11.so.6');U=c.c_ulong;P=c.c_void_p
    x.XOpenDisplay.argtypes=[c.c_char_p];x.XOpenDisplay.restype=P
    x.XDefaultRootWindow.argtypes=[P];x.XDefaultRootWindow.restype=U
    x.XInternAtom.argtypes=[P,c.c_char_p,c.c_int];x.XInternAtom.restype=U
    x.XQueryTree.argtypes=[P,U,c.POINTER(U),c.POINTER(U),c.POINTER(c.POINTER(U)),c.POINTER(c.c_uint)]
    x.XGetWindowProperty.argtypes=[P,U,U,c.c_long,c.c_long,c.c_int,U,c.POINTER(U),c.POINTER(c.c_int),c.POINTER(U),c.POINTER(U),c.POINTER(P)]
    x.XFetchName.argtypes=[P,U,c.POINTER(c.c_char_p)]
    x.XFree.argtypes=[P];x.XFlush.argtypes=[P];x.XCloseDisplay.argtypes=[P]
    d=x.XOpenDisplay(env.get('DISPLAY','').encode());assert d,'X11 display unavailable'
    pidAtom=x.XInternAtom(d,b'_NET_WM_PID',0);delete=x.XInternAtom(d,b'WM_DELETE_WINDOW',0);protocol=x.XInternAtom(d,b'WM_PROTOCOLS',0)
    def windows(window,depth=0):
     if depth>8:return []
     result=[];actual=U();fmt=c.c_int();n=U();after=U();data=P()
     if x.XGetWindowProperty(d,window,pidAtom,0,1,0,0,c.byref(actual),c.byref(fmt),c.byref(n),c.byref(after),c.byref(data))==0:
      if data and fmt.value==32 and n.value and c.cast(data,c.POINTER(U))[0]==placement['pid']:
       result.append(window)
      if data:x.XFree(data)
     root=U();parent=U();children=c.POINTER(U)();count=c.c_uint()
     if x.XQueryTree(d,window,c.byref(root),c.byref(parent),c.byref(children),c.byref(count)):
      ids=[children[i] for i in range(count.value)]
      if children:x.XFree(c.cast(children,P))
      for child in ids:result.extend(windows(child,depth+1))
     return result
    matches=windows(x.XDefaultRootWindow(d));assert len(matches)==1,('ambiguous test windows',matches)
    class Data(c.Union):_fields_=[('b',c.c_char*20),('s',c.c_short*10),('l',c.c_long*5)]
    class Client(c.Structure):_fields_=[('type',c.c_int),('serial',U),('send_event',c.c_int),('display',P),('window',U),('message_type',U),('format',c.c_int),('data',Data)]
    class Event(c.Union):_fields_=[('client',Client),('pad',c.c_long*24)]
    x.XSendEvent.argtypes=[P,U,c.c_int,c.c_long,c.POINTER(Event)]
    event=Event();event.client.type=33;event.client.send_event=1;event.client.display=d;event.client.window=matches[0];event.client.message_type=protocol;event.client.format=32;event.client.data.l[0]=delete
    assert x.XSendEvent(d,matches[0],0,0,c.byref(event));x.XFlush(d);x.XCloseDisplay(d)

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--bundle',type=Path,required=True)
 parser.add_argument('--desktop-environment',type=Path,required=True)
 args=parser.parse_args();b=args.bundle.resolve();env=os.environ.copy()
 env.update(json.loads(args.desktop_environment.read_text()))
 os.environ.update(env)
 label=str(time.time_ns())
 os.sched_setaffinity(0,{6,7})
 for mode in ['healthy','frozen']:
  start=time.time_ns();log=b/('check-'+label+'-'+mode+'.log')
  with log.open('x') as out:
   command=['python3',str(b/'run-test.py'),'candidate']
   if mode=='healthy':command.append('--no-panel')
   process=subprocess.Popen(command,env=env,stdout=out,stderr=subprocess.STDOUT)
  placement=None;placement_path=None;deadline=time.monotonic()+100
  while time.monotonic()<deadline and process.poll() is None:
   for path in (b/'logs').glob('*.placement.json'):
    if path.stat().st_mtime_ns<start:continue
    try:row=json.loads(path.read_text())
    except ValueError:continue
    if row.get('ready_ns') and row.get('panic_control_verified'):
     placement=row;placement_path=path;break
   if placement:break
   time.sleep(.1)
  assert placement,mode+': protected startup failed; inspect '+str(log)
  events=[]
  recovery=PanicRecovery(placement['pid'],b/'Standalone/Aeolus-Profile',placement['panic_socket'],
                         lambda event,**data:events.append({'event':event,**data}),allow_stop=True)
  try:
   if mode=='healthy':
    recovery.request();deadline=time.monotonic()+3
    while recovery.state=='pending' and time.monotonic()<deadline:
     recovery.tick();time.sleep(.025)
    assert recovery.state=='muted' and recovery.alive(),'Healthy panic not acknowledged'
    recovery.client.send('RESUME');deadline=time.monotonic()+20
    while time.monotonic()<deadline and recovery.alive():
     recovery.client.poll();recovery.client.send('STATUS')
     if not recovery.client.muted:break
     time.sleep(.05)
    assert recovery.alive() and not recovery.client.muted,'Audio did not resume'
    close_window(placement['pid'],env)
    assert process.wait(timeout=30)==0,'Healthy app did not close normally'
   else:
    signal.pidfd_send_signal(recovery.pidfd,signal.SIGSTOP)
    result=process.wait(timeout=25)
    assert result in (247,241),'Frozen app was not stopped: '+str(result)
    assert not recovery.alive(),'Frozen process survived'
  finally:recovery.close()
  timing=Path(placement['timing_file']);watch=timing.with_suffix('.watchdog.jsonl')
  watcher=[json.loads(line) for line in watch.read_text().splitlines()]
  names=[row['event'] for row in watcher]
  if mode=='frozen':
   assert 'panic_requested' in names and 'panic_unacknowledged' in names and 'panic_sigterm_sent' in names,names
  else:assert 'panic_sigterm_sent' not in names,names
  # Safety may arm before the background writer publishes its first CSV row.
  # Read diagnostic settings after the test, without gating the safety path.
  latest_placement=json.loads(placement_path.read_text())
  actual_audio=latest_placement.get('first_callback')
  if actual_audio is None:
   with timing.open() as stream:
    row=next(r for r in csv.DictReader(stream) if r.get('sample_rate') and r.get('frames'))
   actual_audio={'sample_rate':row['sample_rate'],'frames':row['frames']}
  proof={'mode':mode,'notes_sent':False,'audio':actual_audio,
         'panic_control_verified':placement['panic_control_verified'],'control_events':events,
         'watchdog_events':names,'launcher_exit':process.returncode}
  (b/('check-'+label+'-'+mode+'-proof.json')).write_text(json.dumps(proof,indent=2))
  print(json.dumps(proof),flush=True)
 print('SILENT PANIC/RESUME AND FROZEN-PROCESS RECOVERY PASSED',flush=True)

if __name__=='__main__':main()
