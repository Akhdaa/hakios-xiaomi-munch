#!/usr/bin/env python3
"""Build a RAM payload that asks iOS to install one local IPA after boot."""
import argparse
import importlib
import ipaddress
import plistlib
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

MAX_IPA = 32 * 1024 * 1024
MAX_PAYLOAD = 63 * 1024 * 1024


def ssh_keys(directory):
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name in ('client', 'host'):
        key = directory / name
        if not key.exists():
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True)
        if not key.with_suffix('.pub').is_file():
            raise ValueError(f'Missing public key: {key}.pub')


def bundle_id(path):
    if path.suffix.lower() != '.ipa' or path.stat().st_size > MAX_IPA:
        raise ValueError('Choose an IPA of at most 32 MiB')
    with zipfile.ZipFile(path) as ipa:
        manifests = [i for i in ipa.infolist() if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', i.filename)]
        if len(manifests) != 1 or manifests[0].file_size > 1024 * 1024:
            raise ValueError('IPA must contain one application Info.plist')
        info = plistlib.loads(ipa.read(manifests[0]))
        identifier = info.get('CFBundleIdentifier', '')
        if not isinstance(identifier, str) or not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', identifier):
            raise ValueError('Invalid app bundle identifier')
        return identifier


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipa', type=Path, required=True)
    parser.add_argument('--ssh-keys', type=Path, required=True)
    parser.add_argument('--live', type=ipaddress.IPv4Address)
    for arg in ('base', 'root', 'installer', 'output'):
        parser.add_argument('--' + arg, type=Path)
    args = parser.parse_args()
    identifier = bundle_id(args.ipa)
    if args.live:
        key = args.ssh_keys / 'client'
        host = args.ssh_keys / 'host.pub'
        if not key.is_file() or not host.is_file():
            parser.error('Boot option 6 once to provision SSH access')
        with tempfile.TemporaryDirectory() as tmp:
            known = Path(tmp) / 'known_hosts'
            known.write_text(f'{args.live} {host.read_text().strip()}\n')
            ssh = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
                   '-o', 'StrictHostKeyChecking=yes', '-o', f'UserKnownHostsFile={known}',
                   '-o', 'GlobalKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=8',
                   '-i', str(key), f'root@{args.live}']
            command = ('set -e; test -x /redwood/bin/munch_ipa_install; '
                       'dir=$(mktemp -d /private/var/tmp/hakios-upload.XXXXXX); '
                       'trap \'rm -f "$dir/test.ipa"; rmdir "$dir"\' EXIT; '
                       f'cat > "$dir/test.ipa"; test "$(wc -c < "$dir/test.ipa")" -eq {args.ipa.stat().st_size}; '
                       f'/redwood/bin/munch_ipa_install "$dir/test.ipa" {identifier}')
            with args.ipa.open('rb') as ipa:
                subprocess.run([*ssh, command], stdin=ipa, check=True)
        return
    if any(getattr(args, arg) is None for arg in ('base', 'root', 'installer', 'output')):
        parser.error('Boot packaging requires --base, --root, --installer and --output')
    payload = importlib.import_module('munch-storage-payload')
    entries = dict(payload.entries(args.base.read_bytes()))
    ssh_keys(args.ssh_keys)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as tmp:
        tmp = Path(tmp)
        root = tmp / 'root'
        shutil.copytree(args.root, root, symlinks=True)
        (root / 'install').mkdir()
        shutil.copyfile(args.ipa, root / 'install/test.ipa')
        shutil.copy2(args.installer, root / 'bin/munch_ipa_install')
        keys = root / 'var/root/.ssh'
        keys.mkdir(mode=0o700, parents=True, exist_ok=True)
        shutil.copyfile(args.ssh_keys / 'client.pub', keys / 'authorized_keys')
        shutil.copyfile(args.ssh_keys / 'host', keys / 'ssh_host_ed25519_key')
        for name in ('authorized_keys', 'ssh_host_ed25519_key'):
            (keys / name).chmod(0o600)
        with (root / 'etc/var.skel').open('a') as skeleton:
            skeleton.write('d\t700\t0\t0\t/private/var/root/.ssh\n')
            for name in ('authorized_keys', 'ssh_host_ed25519_key'):
                skeleton.write(f'f\t600\t0\t0\t/private/var/root/.ssh/{name}\t/var/root/.ssh/{name}\n')
        jobs_path = root / 'etc/rc.jobs'
        with jobs_path.open('a') as jobs:
            jobs.write('\n/var/jb/usr/sbin/sshd -D -e -h /var/root/.ssh/ssh_host_ed25519_key '
                       '-o UsePAM=no -o PasswordAuthentication=no -o PermitRootLogin=prohibit-password '
                       '-o AuthorizedKeysFile=/var/root/.ssh/authorized_keys &\n')
        job = {
            'Label': 'com.hakios.ipa-install',
            'ProgramArguments': ['/redwood/bin/munch_ipa_install', '/redwood/install/test.ipa', identifier],
            'RunAtLoad': True,
            'StandardOutPath': '/dev/console',
            'StandardErrorPath': '/dev/console',
        }
        (root / 'LaunchDaemons/com.hakios.ipa-install.plist').write_bytes(plistlib.dumps(job))
        disk = tmp / 'root.apfs'
        subprocess.run(['python3', 'tools/apfs/mkapfs.py', '-z', '--plain', 'fw', '--plain', 'install',
                        '-o', str(disk), f'redwood={root}'], check=True)
        entries[b'ramdisk'] = disk.read_bytes()
        body = b''.join(payload.entry(name, data) for name, data in entries.items()) + bytes(24)
        if len(body) + 8 > MAX_PAYLOAD:
            raise ValueError('IPA and base image exceed the loader limit')
        args.output.write_bytes(b'XNUP' + len(body).to_bytes(4, 'little') + body)
    print(f'IPA staged for install on boot: {identifier}')


if __name__ == '__main__':
    main()
