import numpy as np

class Grid():
    """
    Base class to implement the grid

    Parameters
    ----------
    shape : tuple(int, ...)
        Size of the grid along each axis (Z, X [, Y])
    """
    def __init__(self, shape):
        self.data = np.zeros(shape, dtype=np.float32)

    def shape(self):
        return self.data.shape

    def add_center_source(self):

        if len(self.shape()) == 2:

            wavelet = [0.016387336, -0.041464937, -0.067372555, 0.386110067,
                       0.812723635, 0.416998396,  0.076488599,  -0.059434419,
                       0.023680172, 0.005611435,  0.001823209,  -0.000720549]

            for s in range(11,0,-1):
                for i in range (self.shape()[0]//2 - s, self.shape()[0]//2 + s - 1, 1):
                    for j in range (self.shape()[1]//2 - s, self.shape()[1]//2 + s -1, 1):
                        self.data[i,j] = wavelet[s]

        else:
            val = 1.0;
            for s in range(5,0,-1):
                for i in range (self.shape()[0]//2 - s, self.shape()[0]//2 + s - 1, 1):
                    for j in range (self.shape()[1]//2 - s, self.shape()[1]//2 + s -1, 1):
                        for k in range (self.shape()[2]//2 - s, self.shape()[2]//2 + s -1, 1):
                            self.data[i,j,k] = val;
                val *= 10;
