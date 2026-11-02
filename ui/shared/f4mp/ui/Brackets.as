package f4mp.ui
{
	import flash.display.Shape;

	// The game's bracket frames: one-pixel lines on the pixel's middle (so they stay one pixel and their shadow matches the
	// text's), drawn inside the box they frame, with the hard shadow.
	public final class Brackets
	{
		// Along the top and the bottom, each with its ends turned in: a panel's frame.
		public static function Panel(a_width:Number, a_height:Number, a_tick:Number, a_color:uint, a_scale:Number):Shape
		{
			var half:Number = 0.5;
			var tick:Number = Math.round(a_tick);
			var shape:Shape = Line(a_color);
			shape.graphics.moveTo(half, half + tick);
			shape.graphics.lineTo(half, half);
			shape.graphics.lineTo(a_width - half, half);
			shape.graphics.lineTo(a_width - half, half + tick);
			shape.graphics.moveTo(half, a_height - half - tick);
			shape.graphics.lineTo(half, a_height - half);
			shape.graphics.lineTo(a_width - half, a_height - half);
			shape.graphics.lineTo(a_width - half, a_height - half - tick);
			shape.filters = [ Style.Shadow(a_scale) ];
			return shape;
		}

		// Down the left and the right, each with its ends turned in: a button hint's "[ ]". The whole box takes the mouse.
		public static function Hint(a_width:Number, a_height:Number, a_tick:Number, a_color:uint, a_scale:Number):Shape
		{
			var half:Number = 0.5;
			var tick:Number = Math.round(a_tick);
			var shape:Shape = new Shape();
			shape.graphics.beginFill(Style.BLACK, 0.001);
			shape.graphics.drawRect(0, 0, a_width, a_height);
			shape.graphics.endFill();
			shape.graphics.lineStyle(1, a_color, 1, true, "normal", "none", "miter");
			shape.graphics.moveTo(half + tick, half);
			shape.graphics.lineTo(half, half);
			shape.graphics.lineTo(half, a_height - half);
			shape.graphics.lineTo(half + tick, a_height - half);
			shape.graphics.moveTo(a_width - half - tick, half);
			shape.graphics.lineTo(a_width - half, half);
			shape.graphics.lineTo(a_width - half, a_height - half);
			shape.graphics.lineTo(a_width - half - tick, a_height - half);
			shape.filters = [ Style.Shadow(a_scale) ];
			return shape;
		}

		private static function Line(a_color:uint):Shape
		{
			var shape:Shape = new Shape();
			shape.graphics.lineStyle(1, a_color, 1, true, "normal", "none", "miter");
			return shape;
		}
	}
}
