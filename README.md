# Scroll

A small program to create a scrolling desktop background.

## Usage

```
scroll [-h|-v] [-b] [-r BEZIER RESOLUTION] [-f FPS] [-V VELOCITY] [-i IMAGE] [-s SCALE] [-p POINTS]
```

Where POINTS is a comma-separated list of x and y coordinates, which specify the path along which to move the image.
The coordinates 0,0 represent the top-left corner and the coordinates 1,1 represent the bottom-right corner of the image.

```
-p x0,y0;x1,y1;x2,y2;...
```

If the -b option is specified the path is smoothed using a bezier curve with the resolution specified by -r (default: 15 points per corner).

## Example

```
scroll -i $IMAGE -s 1.2 -V 0.08 -p '0,0.5;1,0.5'
```

Slowly scrolls $IMAGE back and forth.