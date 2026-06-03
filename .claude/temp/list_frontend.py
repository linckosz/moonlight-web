import os
fw_dir = r'd:\Code\moonlight-web-deepseek\frontend\js\ui'
for f in sorted(os.listdir(fw_dir)):
    path = os.path.join(fw_dir, f)
    mtime = os.path.getmtime(path)
    import datetime
    print(f"{datetime.datetime.fromtimestamp(mtime)}  {f}")
