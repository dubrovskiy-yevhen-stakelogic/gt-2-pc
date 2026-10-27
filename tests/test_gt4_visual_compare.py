"""Synthetic checks for the visual audit's capture validation and hole ranking."""
import sys,tempfile,unittest
from pathlib import Path
from PIL import Image,ImageDraw
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gt4_compare_visuals import inspect

class VisualChecks(unittest.TestCase):
    def check_image(self,paint):
        with tempfile.TemporaryDirectory() as folder:
            image=Image.new('RGB',(64,64),(26,178,230));paint(ImageDraw.Draw(image))
            path=Path(folder)/'frame.png';image.save(path);return inspect(path)
    def test_empty_frame_is_rejected(self):
        self.assertTrue(self.check_image(lambda draw:None)['empty'])
    def test_enclosed_background_hole(self):
        def paint(d):
            d.rectangle((10,10,53,53),fill='red');d.rectangle((25,25,30,30),fill=(26,178,230))
        result=self.check_image(paint)
        self.assertFalse(result['empty']);self.assertFalse(result['cropped'])
        self.assertEqual(result['holePixels'],36)
    def test_open_aperture_is_not_called_enclosed(self):
        def paint(d):
            d.rectangle((10,10,53,53),fill='red');d.rectangle((25,0,30,30),fill=(26,178,230))
        self.assertEqual(self.check_image(paint)['holePixels'],0)
    def test_cut_off_capture_is_rejected(self):
        self.assertTrue(self.check_image(lambda d:d.rectangle((10,10,63,53),fill='red'))['cropped'])

if __name__=='__main__':unittest.main()
