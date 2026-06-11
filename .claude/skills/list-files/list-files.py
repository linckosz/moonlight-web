import subprocess, sys, os
subprocess.run([os.environ.get('COMSPEC','cmd'), '/c', 'dir', sys.argv[1]], shell=True)
