import json,re,subprocess,sys
r=subprocess.run(['python3','tv.py','shell','uiautomator dump /sdcard/ui.xml >/dev/null && cat /sdcard/ui.xml'],capture_output=True,text=True,cwd=sys.path[0])
x=json.loads(r.stdout).get('stdout','')
for n in re.findall(r'<node [^>]*>',x):
    t=re.search(r'text="([^"]*)"',n).group(1); d=re.search(r'content-desc="([^"]*)"',n).group(1)
    f='focused="true"' in n; c=re.search(r'checked="(\w+)"',n).group(1); ck='checkable="true"' in n
    if t or d or f: print(('>> ' if f else '   ')+(t or d)+(f' [checked={c}]' if ck else ''))
