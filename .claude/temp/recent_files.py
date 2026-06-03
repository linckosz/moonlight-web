import os, time

backend_dir = r'd:\Code\moonlight-web-deepseek\backend\src'
now = time.time()
for dirpath, dirnames, filenames in os.walk(backend_dir):
    for f in filenames:
        if f.endswith(('.cpp', '.h', '.hpp')):
            path = os.path.join(dirpath, f)
            mtime = os.path.getmtime(path)
            age_hours = (now - mtime) / 3600
            if age_hours < 48:  # modified in last 48 hours
                print(f"{age_hours:5.1f}h  {path}")
