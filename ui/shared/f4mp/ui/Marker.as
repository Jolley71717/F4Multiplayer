package f4mp.ui
{
	import flash.display.Shape;

	// A player's compass marker (Compass.cpp's DrawShape), centered on (0, 0), with no outline: the multiplayer menu's player
	// list and the developer HUD draw it as the compass does. a_size: of the compass's own.
	public final class Marker
	{
		public static function Draw(a_color:uint, a_shape:int, a_size:Number):Shape
		{
			var shape:Shape = new Shape();
			shape.graphics.beginFill(a_color);
			switch (a_shape) {
				case 0:
					shape.graphics.drawCircle(0, 0, 6 * a_size);
					break;
				case 1:
					shape.graphics.drawRect(-5.5 * a_size, -5.5 * a_size, 11 * a_size, 11 * a_size);
					break;
				case 2:
					shape.graphics.moveTo(0, -7.5 * a_size);
					shape.graphics.lineTo(6.5 * a_size, 0);
					shape.graphics.lineTo(0, 7.5 * a_size);
					shape.graphics.lineTo(-6.5 * a_size, 0);
					shape.graphics.lineTo(0, -7.5 * a_size);
					break;
				default:
					shape.graphics.moveTo(0, 6.5 * a_size);
					shape.graphics.lineTo(7 * a_size, -6 * a_size);
					shape.graphics.lineTo(-7 * a_size, -6 * a_size);
					shape.graphics.lineTo(0, 6.5 * a_size);
					break;
			}
			shape.graphics.endFill();
			return shape;
		}
	}
}
