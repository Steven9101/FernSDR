"""Live nonce canaries, using only the exclusively owned lab and local netns."""
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import time
import uuid


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=5).stdout


def helper(mode, address, port, nonce):
    if mode == 'serve':
        with socket.socket() as sock:
            sock.bind((address, 0))
            sock.listen(8)
            sock.settimeout(1)
            print(sock.getsockname()[1], flush=True)
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                try:
                    connection, _ = sock.accept()
                except TimeoutError:
                    continue
                with connection:
                    connection.settimeout(.5)
                    try:
                        if connection.recv(128) == nonce.encode():
                            connection.sendall(nonce.encode())
                    except OSError:
                        pass
    else:
        connected = False
        try:
            with socket.create_connection((address, int(port)), timeout=.5) as sock:
                connected = True
                sock.sendall(nonce.encode())
                reply = sock.recv(128)
                print(json.dumps({'connected': True, 'nonce_ok': reply == nonce.encode()}))
        except (TimeoutError, ConnectionRefusedError) as error:
            print(json.dumps({'connected': connected, 'nonce_ok': False, 'reason': type(error).__name__}))


def check(receiver_pid, client='fbc1'):
    if client != 'fbc1' or not str(receiver_pid).isdigit():
        raise ValueError('expected owned lab receiver PID and fbc1')
    host_ip, outside_ip = '198.18.253.1', '198.18.254.2'
    outside = 'fbtest-' + uuid.uuid4().hex[:6]
    link = 'fbx' + uuid.uuid4().hex[:6]
    nonce = uuid.uuid4().hex
    script = str(Path(__file__).resolve())
    processes, cleanup, rows = [], [], []
    receiver_fd = os.open(f'/proc/{receiver_pid}/ns/net', os.O_RDONLY)
    prefixes = {'host': [], 'receiver': ['nsenter', f'--net=/proc/{os.getpid()}/fd/{receiver_fd}'],
                'client': ['ip', 'netns', 'exec', client], 'outside': ['ip', 'netns', 'exec', outside]}
    addresses = {'host': host_ip, 'receiver': '198.18.0.2', 'client': '198.18.1.1', 'outside': outside_ip}
    try:
        rules = json.loads(command('nft', '-j', 'list', 'table', 'inet', 'fernbench'))
        chains = {entry['rule']['chain'] for entry in rules['nftables'] if 'rule' in entry
                  and any('drop' in expr for expr in entry['rule']['expr'])}
        if chains != {'input', 'output', 'forward'}:
            raise RuntimeError('lab drop rules missing')
        if any(x.get('addr_info') for x in json.loads(command('ip', '-j', 'addr', 'show', 'br-fb'))):
            raise RuntimeError('bridge already has an address')
        for name in ('receiver', 'client'):
            if command(*prefixes[name], 'ip', 'route', 'show', 'default').strip():
                raise RuntimeError(f'{name} has a default route')
            if command(*prefixes[name], 'sysctl', '-n', 'net.ipv6.conf.all.disable_ipv6').strip() != '1':
                raise RuntimeError(f'{name} has IPv6 enabled')
        import ipaddress
        lab_range = ipaddress.ip_network('198.18.0.0/16')
        for route in json.loads(command('ip', '-j', '-4', 'route', 'show', 'table', 'all')):
            destination = route.get('dst', 'default')
            if destination != 'default' and ipaddress.ip_network(destination).overlaps(lab_range):
                raise RuntimeError('host already routes a lab address range')
        command('ip', 'addr', 'add', host_ip + '/16', 'dev', 'br-fb')
        cleanup.append(['ip', 'addr', 'del', host_ip + '/16', 'dev', 'br-fb'])
        command('ip', 'netns', 'add', outside)
        cleanup.append(['ip', 'netns', 'del', outside])
        command('ip', 'link', 'add', link, 'type', 'veth', 'peer', 'name', link + 'p')
        cleanup.append(['ip', 'link', 'del', link])
        command('ip', 'link', 'set', link + 'p', 'netns', outside)
        command('ip', 'addr', 'add', '198.18.254.1/30', 'dev', link)
        command('ip', 'link', 'set', link, 'up')
        command(*prefixes['outside'], 'ip', 'addr', 'add', outside_ip + '/30', 'dev', link + 'p')
        command(*prefixes['outside'], 'ip', 'link', 'set', link + 'p', 'up')
        command(*prefixes['outside'], 'ip', 'link', 'set', 'lo', 'up')
        command(*prefixes['outside'], 'ip', 'route', 'add', '198.18.0.0/16', 'via', '198.18.254.1')
        for name in ('receiver', 'client'):
            route = [*prefixes[name], 'ip', 'route', 'add', outside_ip + '/32', 'via', host_ip]
            command(*route)
            cleanup.append([*prefixes[name], 'ip', 'route', 'del', outside_ip + '/32', 'via', host_ip])
        ports = {}
        for name in prefixes:
            process = subprocess.Popen([*prefixes[name], sys.executable, script, 'serve', addresses[name], '0', nonce],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(process)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(3):
                    raise RuntimeError(f'{name} canary did not start')
            ports[name] = int(process.stdout.readline())
        def probe(source, target, allowed):
            before = command('nft', '-j', 'list', 'table', 'inet', 'fernbench')
            value = json.loads(command(*prefixes[source], sys.executable, script, 'probe',
                                       addresses[target], str(ports[target]), nonce))
            after = command('nft', '-j', 'list', 'table', 'inet', 'fernbench')
            chain = 'output' if source == 'host' else 'input' if target == 'host' else 'forward'
            def dropped(raw):
                return sum(expr['counter']['packets'] for entry in json.loads(raw)['nftables']
                           if 'rule' in entry and entry['rule']['chain'] == chain
                           for expr in entry['rule']['expr'] if 'counter' in expr)
            delta = dropped(after) - dropped(before)
            ok = value.get('nonce_ok') is True if allowed else value.get('connected') is False and delta > 0
            rows.append({'source': source, 'target': target, 'allowed': allowed, 'result': value,
                         'drop_chain': chain, 'drop_counter_delta': delta, 'ok': ok})
            if not ok:
                raise RuntimeError(f'isolation canary failed: {rows[-1]}')
        for name in prefixes:
            probe(name, name, True)
        probe('receiver', 'client', True)
        probe('client', 'receiver', True)
        for domain in ('host', 'outside'):
            for endpoint in ('receiver', 'client'):
                probe(domain, endpoint, False)
                probe(endpoint, domain, False)
        for name in prefixes:
            probe(name, name, True)
        return {'ok': True, 'scope': 'host input/output and cross-interface forwarding; non-lab destination filter not probed', 'cases': rows, 'receiver_namespace': os.fstat(receiver_fd).st_ino}
    finally:
        errors = []
        for process in processes:
            try:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=2)
            except Exception as error:
                errors.append(str(error))
        for args in reversed(cleanup):
            try:
                command(*args)
            except Exception as error:
                errors.append(str(error))
        try:
            if any(x.get('addr_info') for x in json.loads(command('ip', '-j', 'addr', 'show', 'br-fb'))):
                errors.append('canary bridge address remains after cleanup')
        except Exception as error:
            errors.append(str(error))
        os.close(receiver_fd)
        if errors:
            raise RuntimeError('canary cleanup failed: ' + '; '.join(errors))


if __name__ == '__main__':
    helper(*sys.argv[1:])
