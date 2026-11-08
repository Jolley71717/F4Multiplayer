package f4mp.ui
{
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.events.MouseEvent;

	// A scrollbar's handle, as tall as the part shown is of the whole, on the left of the frame's right side (Brackets.Scrolling), which is the bar's line, so
	// put this at that line. The mouse takes a strip along it: a_onDrag(fraction), 0 at the top and 1 at the end, while held.
	public class ScrollBar extends Sprite
	{
		private static const HANDLE:Number = 18;
		private static const MIN_HANDLE:Number = 8;
		private static const STRIP_WIDTH:Number = 28;

		private var thumb:Shape = new Shape();
		private var onDrag:Function;
		private var barHeight:Number = 0;
		private var size:Number = 0;
		private var thumbHeight:Number = 0;
		private var color:uint = 0;

		public function ScrollBar(a_onDrag:Function, a_height:Number, a_color:uint, a_scale:Number)
		{
			super();
			onDrag = a_onDrag;
			barHeight = a_height;
			size = Math.max(MIN_HANDLE, Math.round(HANDLE * a_scale));
			var strip:Number = STRIP_WIDTH * a_scale;
			graphics.beginFill(Style.BLACK, 0.001);
			graphics.drawRect(1 - strip, 0, strip, a_height);
			graphics.endFill();
			color = a_color;
			addChild(thumb);
			buttonMode = true;
			addEventListener(MouseEvent.MOUSE_DOWN, Down);
			addEventListener(Event.REMOVED_FROM_STAGE, Removed);
		}

		// a_shown: the part of the whole that shows (0 to 1). a_at: how far along it is (0 to 1).
		public function Update(a_shown:Number, a_at:Number):void
		{
			thumbHeight = Math.round(Math.min(barHeight, Math.max(size, barHeight * a_shown)));
			thumb.graphics.clear();
			thumb.graphics.beginFill(color, 1);
			thumb.graphics.drawRect(1 - size, 0, size, thumbHeight);
			thumb.graphics.endFill();
			thumb.y = Math.round((barHeight - thumbHeight) * Math.max(0, Math.min(1, a_at)));
		}

		private function Down(a_e:MouseEvent):void
		{
			if (stage != null) {
				stage.addEventListener(MouseEvent.MOUSE_MOVE, Drag);
				stage.addEventListener(MouseEvent.MOUSE_UP, Up);
			}
			Drag(a_e);
		}

		private function Drag(a_e:MouseEvent):void
		{
			var room:Number = barHeight - thumbHeight;
			onDrag(room > 0 ? Math.max(0, Math.min(1, (mouseY - thumbHeight / 2) / room)) : 0);
		}

		private function Up(a_e:MouseEvent):void
		{
			Stop();
		}

		private function Removed(a_e:Event):void
		{
			Stop();
		}

		private function Stop():void
		{
			if (stage != null) {
				stage.removeEventListener(MouseEvent.MOUSE_MOVE, Drag);
				stage.removeEventListener(MouseEvent.MOUSE_UP, Up);
			}
		}
	}
}
