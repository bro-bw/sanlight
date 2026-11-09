from pathlib import Path
import sys
import numpy as np
from PIL import Image
root = Path(__file__).resolve().parents[2] / 'build'
im = np.asarray(Image.open(sys.argv[2]).convert('RGBA'), dtype=np.float32) / 255
h, w = im.shape[:2]
if sys.argv[1] == 'input':
    data = im.copy()
    data[:, :, :3] = data[:, :, :3] * data[:, :, 3:] + .5 * (1-data[:, :, 3:])
    with (root/'probe_input.bin').open('wb') as f:
        np.array([w,h], dtype='<i4').tofile(f)
        data.astype('<f4').tofile(f)
else:
    data = np.fromfile(root/'probe_output.bin', dtype='<f4')
    depth = data[:w*h].reshape(h,w)
    normal = data[w*h:].reshape(h,w,3)
    for y in [100, 150, 180, 200, 350]:
        print('row', y, [(x, round(float(depth[y,x]),3), round(float(normal[y,x,0]*2-1),3)) for x in [330,350,375,400,425,450,470]])
    print('background depth',float(depth[0,0]), 'nose',float(depth[190,400]))
    # Regression points on the supplied character's left and right cheeks.
    assert depth[190,400] > depth[0,0]
    points = [(350,150), (450,150)]
    def diffuse(light_x):
        values = []
        for x,y in points:
            n = normal[y,x]*2-1
            n[1] *= -1
            light = np.array([light_x-x, 150-y, 700+1000*(1-depth[y,x])])
            values.append(max(0, float(np.dot(n, light/np.linalg.norm(light)))))
        return values
    left, right = diffuse(-300), diffuse(1100)
    assert left[0] > left[1] and right[1] > right[0], (left, right)
    print('PASS left lamp / cheek brightness:', left, 'right lamp:', right)
    Image.fromarray((depth*255).astype('uint8')).save(root/'probe_depth.png')
    Image.fromarray((np.clip(normal,0,1)*255).astype('uint8')).save(root/'probe_normal.png')
