package f4mp.ui
{
	import flash.display.Sprite;

	// The mod's logo (logo.svg, as LogoArt) at a height, in a color, its top left at (0, 0).
	public class Logo extends Sprite
	{
		public function Logo(a_height:Number, a_color:uint)
		{
			super();
			var size:Number = a_height / LogoArt.HEIGHT;
			graphics.beginFill(a_color);
			for (var i:int = 0; i < LogoArt.OUTLINES.length; ++i) {
				var points:Array = LogoArt.OUTLINES[i];
				graphics.moveTo(points[0] * size, points[1] * size);
				for (var p:int = 2; p < points.length; p += 2) {
					graphics.lineTo(points[p] * size, points[p + 1] * size);
				}
				graphics.lineTo(points[0] * size, points[1] * size);
			}
			graphics.endFill();
		}
	}
}
