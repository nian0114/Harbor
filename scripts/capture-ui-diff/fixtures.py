"""Disposable macOS capture services. Loopback only; standard library by default."""
import contextlib
import getpass
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import threading
import time


class FixtureError(RuntimeError):
    pass


def command(args, timeout=15):
    r = subprocess.run(list(map(str,args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise FixtureError(f'{Path(str(args[0])).name}: {r.stderr.strip() or r.stdout.strip()}')
    return r.stdout.strip()


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]


class SSH:
    """Use macOS's existing OpenSSH daemon with generated keys, never ~/.ssh."""
    def __init__(self, data):
        self.data = Path(data)/'sshd'
        self.data.mkdir(mode=0o700)
        self.port = free_port()
        self.process = None

    def start(self):
        if not Path('/usr/sbin/sshd').is_file():
            raise FixtureError('macOS OpenSSH server is unavailable: /usr/sbin/sshd')
        self.user = getpass.getuser()
        self.key = self.data/'client'
        command(['/usr/bin/ssh-keygen','-q','-t','ed25519','-N','','-f',self.key])
        host = self.data/'host'
        command(['/usr/bin/ssh-keygen','-q','-t','ed25519','-N','','-f',host])
        self.public = Path(str(host)+'.pub').read_text().strip()
        config = self.data/'config'
        config.write_text(f'''Port {self.port}
ListenAddress 127.0.0.1
HostKey {host}
PidFile {self.data / 'pid'}
AuthorizedKeysFile {self.key}.pub
UsePAM no
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
AllowUsers {self.user}
AllowTcpForwarding yes
PermitTTY yes
Subsystem sftp internal-sftp
SetEnv ZDOTDIR={self.data}
LogLevel ERROR
''')
        self.log = (self.data/'server.log').open('w+')
        self.process = subprocess.Popen(['/usr/sbin/sshd','-D','-e','-f',str(config)],
                                        stdout=self.log,stderr=subprocess.STDOUT)
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise FixtureError('OpenSSH fixture exited: '+self.log.read(2000))
            try:
                with socket.create_connection(('127.0.0.1',self.port),timeout=.1):return self
            except OSError:time.sleep(.03)
        raise FixtureError('OpenSSH fixture did not start')

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        if hasattr(self,'log'):self.log.close()


class Gate:
    """Real TCP relay: delay handshake or throttle SFTP to expose transient states."""
    def __init__(self, target, delay=0, interval=0):
        self.target, self.delay, self.interval = target, delay, interval
        self.stop = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1',0));self.listener.listen();self.listener.settimeout(.2)
        self.port = self.listener.getsockname()[1]
        self.sockets = []
        threading.Thread(target=self.accept,daemon=True).start()

    def accept(self):
        while not self.stop.is_set():
            try:client,_=self.listener.accept()
            except OSError:continue
            self.sockets.append(client)
            threading.Thread(target=self.relay,args=(client,),daemon=True).start()

    def relay(self,client):
        try:
            upstream=socket.create_connection(('127.0.0.1',self.target),timeout=3)
            upstream.settimeout(None);self.sockets.append(upstream)
            if self.stop.wait(self.delay):return
            while not self.stop.is_set():
                ready,_,_=select.select([client,upstream],[],[],.2)
                for source in ready:
                    data=source.recv(16384)
                    if not data:return
                    (upstream if source is client else client).sendall(data)
                    if self.interval:self.stop.wait(self.interval)
        except OSError:pass
        finally:
            with contextlib.suppress(OSError):client.close()

    def close(self):
        self.stop.set();self.listener.close()
        for s in self.sockets:
            with contextlib.suppress(OSError):s.shutdown(socket.SHUT_RDWR);s.close()


class VNC:
    """A real RFB 3.8 framebuffer fixture, without changing Screen Sharing settings."""
    def __init__(self):
        self.stop = threading.Event()
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen()
        self.listener.settimeout(.2);self.port=self.listener.getsockname()[1]
        self.sockets=[];self.width,self.height=1024,640
        threading.Thread(target=self.accept,daemon=True).start()

    def accept(self):
        while not self.stop.is_set():
            try:client,_=self.listener.accept()
            except OSError:continue
            self.sockets.append(client)
            threading.Thread(target=self.serve,args=(client,),daemon=True).start()

    def serve(self,s):
        def read(n):
            data=b''
            while len(data)<n:
                part=s.recv(n-len(data))
                if not part:raise EOFError()
                data+=part
            return data
        try:
            s.settimeout(15);s.sendall(b'RFB 003.008\n');read(12)
            s.sendall(b'\x01\x01');security=read(1)
            if security!=b'\x01':return
            s.sendall(struct.pack('>I',0));read(1)
            pixel=struct.pack('>BBBBHHHBBBxxx',32,24,0,1,255,255,255,16,8,0)
            name=b'Harbor isolated macOS VNC capture fixture'
            s.sendall(struct.pack('>HH',self.width,self.height)+pixel+struct.pack('>I',len(name))+name)
            typed=''
            while not self.stop.is_set():
                kind=read(1)[0]
                if kind==0:
                    read(3);pixel=read(16)
                elif kind==2:
                    read(1);read(struct.unpack('>H',read(2))[0]*4)
                elif kind==3:
                    read(9)
                    from PIL import Image,ImageDraw
                    image=Image.new('RGB',(self.width,self.height),(35,31,65));draw=ImageDraw.Draw(image)
                    draw.rectangle((0,0,self.width,40),fill=(24,29,42))
                    draw.text((18,14),'Harbor VNC fixture | macOS client | 127.0.0.1',fill='white')
                    draw.rectangle((90,100,934,510),fill=(10,15,30),outline=(80,95,120),width=2)
                    draw.text((110,122),'Local test desktop - synthetic remote content, real RFB transport',fill=(130,180,240))
                    draw.text((110,170),'capture@fixture:~$ '+typed,fill=(63,217,127))
                    draw.text((110,205),'No personal desktop or remote machine is involved.',fill=(150,160,180))
                    bpp,depth,bigend,truecolor,rmax,gmax,bmax,rshift,gshift,bshift=struct.unpack('>BBBBHHHBBBxxx',pixel)
                    if bpp==32 and (rshift,gshift,bshift)==(16,8,0):
                        raw=image.tobytes('raw','XRGB' if bigend else 'BGRX')
                    else:
                        import array
                        values=array.array('H' if bpp==16 else 'I',(((r*rmax//255)<<rshift)|((g*gmax//255)<<gshift)|((b*bmax//255)<<bshift) for r,g,b in image.getdata()))
                        if bool(bigend)==(os.sys.byteorder=='little'):values.byteswap()
                        raw=values.tobytes()
                    s.sendall(b'\x00\x00\x00\x01'+struct.pack('>HHHHi',0,0,self.width,self.height,0)+raw)
                elif kind==4:
                    down=read(1)[0];read(2);key=struct.unpack('>I',read(4))[0]
                    if down and 32<=key<127:typed+=chr(key)
                    elif down and key==0xff08:typed=typed[:-1]
                    elif down and key==0xff0d:typed=''
                elif kind==5:read(5)
                elif kind==6:read(3);read(struct.unpack('>I',read(4))[0])
                else:return
        except (OSError,EOFError):pass
        finally:
            with contextlib.suppress(OSError):s.close()

    def close(self):
        self.stop.set();self.listener.close()
        for s in self.sockets:
            with contextlib.suppress(OSError):s.shutdown(socket.SHUT_RDWR);s.close()


class RDP:
    """Native macOS FreeRDP server with disposable TLS keys and synthetic pixels."""
    def __init__(self,data):
        self.root=Path(data)/'rdp';self.root.mkdir(mode=0o700)
        self.port=free_port();self.process=None;self.stop=threading.Event()
        self.cert=self.root/'cert.pem';self.key=self.root/'key.pem'
        self.pixels=self.root/'pixels.bgra';self.inputs=self.root/'inputs.txt'
        self.typed='';self.last='';self.menu=False;self.lines=0

    def draw(self):
        from PIL import Image,ImageDraw
        im=Image.new('RGB',(1024,640),(35,72,120));d=ImageDraw.Draw(im)
        d.rectangle((0,0,1024,40),fill=(23,33,49))
        d.text((18,14),'Harbor local RDP protocol fixture | synthetic remote content',fill='white')
        d.rectangle((95,100,930,535),fill=(12,16,28),outline=(130,150,180),width=2)
        d.text((112,119),'Terminal',fill=(150,180,225))
        d.text((112,164),'capture@fixture:~$ '+self.last,fill=(210,225,245))
        if self.last:d.text((112,188),'Loopback RDP input received by the test server.',fill=(90,210,140))
        d.text((112,225),'capture@fixture:~$ '+self.typed,fill=(90,210,140))
        d.rectangle((0,600,1024,640),fill=(20,28,43));d.text((18,615),'Start',fill='white')
        if self.menu:
            d.rectangle((10,370,210,595),fill=(26,36,55),outline=(130,150,180))
            for i,label in enumerate(('Terminal','Settings','Files','Power')):d.text((30,395+i*42),label,fill='white')
        tmp=self.pixels.with_suffix('.tmp');tmp.write_bytes(im.tobytes('raw','BGRX'));tmp.replace(self.pixels)

    def render(self):
        while not self.stop.wait(.1):
            lines=self.inputs.read_text().splitlines() if self.inputs.exists() else []
            for line in lines[self.lines:]:
                try:kind,flags,code=line.split();flags=int(flags);code=int(code)
                except ValueError:continue
                if flags&0x8000:continue
                if kind=='K' and code in (0x5b,0x5c):self.menu=not self.menu
                elif kind=='K' and code==0x1c:self.last=self.typed;self.typed=''
                elif kind=='K' and code==0x0e:self.typed=self.typed[:-1]
                elif kind=='U':
                    if code==13:self.last=self.typed;self.typed=''
                    elif code==8:self.typed=self.typed[:-1]
                    elif code>=32:self.typed+=chr(code)
            self.lines=len(lines);self.draw()

    def start(self):
        binary=Path(__file__).resolve().parents[2]/'.cache/ui-rdp-fixture'
        if not binary.is_file():raise FixtureError('RDP fixture is not built; run scripts/capture-ui-diff/build-fixtures.py')
        command(['/usr/bin/openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',self.key,
                 '-out',self.cert,'-days','1','-subj','/CN=127.0.0.1'])
        self.key.chmod(0o600);self.draw();self.inputs.touch()
        self.log=(self.root/'server.log').open('w+')
        self.process=subprocess.Popen([str(binary),str(self.port),str(self.cert),str(self.key),str(self.pixels),str(self.inputs)],
                                      stdout=self.log,stderr=subprocess.STDOUT)
        self.renderer=threading.Thread(target=self.render,daemon=True)
        self.renderer.start()
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.seek(0);raise FixtureError('RDP fixture exited: '+self.log.read(2000))
            try:
                with socket.create_connection(('127.0.0.1',self.port),timeout=.1):return self
            except OSError:time.sleep(.03)
        raise FixtureError('RDP fixture did not start')

    def close(self):
        self.stop.set()
        if hasattr(self,'renderer'):self.renderer.join(timeout=1)
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        if hasattr(self,'log'):self.log.close()
