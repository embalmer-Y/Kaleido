#!/usr/bin/env python3
"""
llm_probe.py — Kaleido Agent 侧 LLM 端点探针（CCR 风格）

检查链：
  1. URL 合法性（scheme/host，尾缀 /v1 提示）
  2. 连通性 + 鉴权（GET /models：200=OK；401/403=key 无效；超时/DNS=不通）
  3. 模型清单（data[].id）
  4. 逐模型 chat 实测（tiny prompt；2xx+choices=可用；404=模型不存在；401=无权限；超时=不可用）
     推理模型 content 可能为空（token 耗在 reasoning）——按 choices 结构判可用
  5. --apply：把配置写入设备 /api/config（esp-claw 部分字段 JSON 写入）

用法：
  python3 llm_probe.py <base_url> <api_key> [--top N | --probe m1 m2 ...] [--apply <board_ip>] [--model M]
仅标准库，无三方依赖。
"""
import argparse
import concurrent.futures as cf
import json
import ssl
import sys
import time
import urllib.error
import urllib.request

UA = {'User-Agent': 'kaleido-llm-probe/1.0'}


def http_json(method, url, key, body=None, timeout=20):
    """返回 (status, latency_s, obj|raw_str, err)"""
    headers = dict(UA)
    if key:
        headers['Authorization'] = 'Bearer ' + key
    data = None
    if body is not None:
        headers['Content-Type'] = 'application/json'
        data = json.dumps(body).encode()
    req = urllib.request.Request(url, method=method, headers=headers, data=data)
    t0 = time.time()
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        raw = r.read()
        try:
            obj = json.loads(raw)
        except Exception:
            obj = None
        return r.status, time.time() - t0, obj, None
    except urllib.error.HTTPError as e:
        raw = e.read().decode('utf-8', 'replace')[:200]
        return e.code, time.time() - t0, None, raw
    except (urllib.error.URLError, ssl.SSLError, TimeoutError, OSError) as e:
        return None, time.time() - t0, None, str(e)[:200]


def check_url(base):
    p = urllib.request.urlparse(base)
    problems = []
    if p.scheme not in ('http', 'https'):
        problems.append(f'协议应为 http/https（当前 {p.scheme!r}）')
    if not p.netloc:
        problems.append('缺少 host')
    if p.query or p.fragment:
        problems.append('URL 不应带 query/fragment')
    if not base.rstrip('/').endswith('/v1') and '/api' not in base:
        problems.append('提示：常见 OpenAI 兼容端点以 /v1 结尾（如 https://host/api/v1）')
    return problems


def probe_model(base, key, model, timeout):
    body = {'model': model,
            'messages': [{'role': 'user', 'content': '回复：OK'}],
            'max_tokens': 64, 'temperature': 0}
    st, dt, obj, err = http_json('POST', base + '/chat/completions', key, body, timeout)
    if st == 200 and obj:
        ch = (obj.get('choices') or [{}])[0]
        content = (ch.get('message') or {}).get('content') or ''
        reasoning = bool((obj.get('usage') or {}).get('completion_tokens_details', {})
                         if isinstance(obj.get('usage'), dict) else False)
        note = '推理模型(内容为空属正常)' if not content else repr(content[:24])
        return model, '可用', round(dt, 1), note
    if st == 404:
        return model, '不存在', round(dt, 1), err or ''
    if st in (401, 403):
        return model, '无权限', round(dt, 1), err or ''
    if st is None:
        return model, '超时/不通', round(dt, 1), err or ''
    return model, f'异常HTTP{st}', round(dt, 1), err or ''


def apply_to_board(board_ip, base, key, model):
    payload = {'llm_backend_type': 'openai_compatible',
               'llm_base_url': base,
               'llm_api_key': key,
               'llm_model': model,
               'llm_supports_tools': 'true'}
    req = urllib.request.Request(f'http://{board_ip}/api/config', method='POST',
                                 headers={'Content-Type': 'application/json'},
                                 data=json.dumps(payload).encode())
    try:
        r = urllib.request.urlopen(req, timeout=10)
        return r.status, r.read().decode('utf-8', 'replace')[:200]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')[:200]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base_url')
    ap.add_argument('api_key')
    ap.add_argument('--top', type=int, default=5, help='实测 /models 前 N 个（默认 5）')
    ap.add_argument('--probe', nargs='+', help='只实测指定模型')
    ap.add_argument('--timeout', type=float, default=20)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--apply', metavar='BOARD_IP', help='把配置写入设备 /api/config')
    ap.add_argument('--model', help='写入设备的模型名（默认取第一个可用模型）')
    args = ap.parse_args()

    base = args.base_url.rstrip('/')
    print(f'== 1. URL 检查 ==\n  {base}')
    problems = check_url(base)
    for p in problems:
        print('  ⚠', p)
    if any('协议' in p or 'host' in p for p in problems):
        sys.exit('URL 非法，终止')

    print('== 2. 连通性 + 鉴权（GET /models）==')
    st, dt, obj, err = http_json('GET', base + '/models', args.api_key, timeout=args.timeout)
    if st != 200 or not isinstance(obj, dict):
        hint = {401: 'key 无效', 403: 'key 无权限', 404: '端点路径不对（/models 不存在）'}.get(st, '')
        sys.exit(f'  ✗ HTTP={st} {hint} {err or ""}（{dt:.1f}s）')
    models = [m.get('id') for m in obj.get('data', []) if m.get('id')]
    print(f'  ✓ HTTP 200（{dt:.2f}s），模型 {len(models)} 个')

    print('== 3. 模型实测（chat/completions）==')
    targets = args.probe or models[:args.top]
    results = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(probe_model, base, args.api_key, m, args.timeout) for m in targets]
        for f in futs:
            results.append(f.result())
    ok_models = []
    print(f'  {"模型":<44} {"状态":<10} {"延迟":>6}  备注')
    for m, verdict, dt, note in results:
        print(f'  {m:<44} {verdict:<10} {dt:>5}s  {note}')
        if verdict == '可用':
            ok_models.append(m)

    if args.apply:
        chosen = args.model or (ok_models[0] if ok_models else None)
        if not chosen:
            sys.exit('  ✗ 无可用模型可下发')
        print(f'== 4. 下发配置到设备 {args.apply} ==\n  模型: {chosen}')
        st, body = apply_to_board(args.apply, base, args.api_key, chosen)
        print(f'  HTTP {st} {body}')
        print('  完成。设备侧可用 /api/config 复核，或直接 WebIM 对话验证。')


if __name__ == '__main__':
    main()
