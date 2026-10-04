"""Read config/local.env (KEY=value lines, optional quotes) without executing it."""
import os, shlex

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path=None):
    path = path or os.path.join(ROOT, 'config', 'local.env')
    env = {}
    if not os.path.exists(path):
        return env
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        k, v = line.split('=', 1)
        v = v.split(' #', 1)[0].strip()
        env[k.strip()] = shlex.split(v)[0] if v else ''
    return env
