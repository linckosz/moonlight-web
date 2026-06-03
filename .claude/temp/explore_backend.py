import os
root = r"d:\Code\moonlight-web-deepseek\backend\src"
for dirpath, dirnames, filenames in os.walk(root):
    for f in filenames:
        if f.endswith(('.cpp', '.h', '.hpp')):
            print(os.path.join(dirpath, f))
