package f4mp.ui
{
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.text.TextField;
	import flash.utils.getTimer;

	// A box over the panel with a title, a text, a place to type if it wants one, and rows, for anything that asks or tells something
	// and comes back: a popup, a confirmation, a password. The owner gives it the panel's size, keeps the page under it as it was, and
	// sends it the keys (rows). Nothing under it takes the mouse. What a row does is the owner's, as with EntryList.
	public class Dialog extends Sprite
	{
		private static const SHARE:Number = 0.8;     // of the panel's width
		private static const PAD:Number = 26;
		private static const GAP:Number = 12;
		private static const TITLE_SIZE:Number = 34;
		private static const TEXT_SIZE:Number = 28;
		private static const TICK:Number = 14;
		private static const INPUT_PAD:Number = 8;
		private static const INPUT_TICK:Number = 6;
		private static const CARET_WIDTH:Number = 3;
		private static const CARET_EVERY:int = 530;  // ms on, and off; Windows' own
		private static const CATCHER_ALPHA:Number = 0.001;  // takes the mouse, and shows nothing

		public var rows:EntryList;
		private var caret:Shape = null;
		private var caretFrom:int = 0;

		public function Dialog(a_onPress:Function, a_onMove:Function)
		{
			super();
			rows = new EntryList(a_onPress, a_onMove);
		}

		// a_labels: the rows' text fields, as EntryList takes them. a_layout: pitch, barHeight, barInset and textInset, scaled, as EntryList
		// takes them. a_input: "" with a_hasInput for an empty place to type.
		public function Build(a_title:String, a_text:String, a_labels:Array, a_hasInput:Boolean, a_input:String, a_panelWidth:Number, a_panelHeight:Number, a_layout:Object, a_color:uint, a_scale:Number):void
		{
			// Over the page: the mouse stops here.
			graphics.beginFill(Style.BLACK, CATCHER_ALPHA);
			graphics.drawRect(0, 0, a_panelWidth, a_panelHeight);
			graphics.endFill();

			var width:Number = Math.round(a_panelWidth * SHARE);
			var pad:Number = PAD * a_scale;
			var inner:Number = width - pad * 2;
			var box:Sprite = new Sprite();
			var y:Number = pad;

			var title:TextField = Style.Text(a_title, TITLE_SIZE * a_scale, a_color, a_scale, Style.BOLD_FONT);
			title.x = pad;
			title.y = y;
			box.addChild(title);
			y += title.height + GAP * a_scale;

			if (a_text != "") {
				var text:TextField = Style.Wrapped(a_text, inner, TEXT_SIZE * a_scale, a_color, a_scale);
				text.x = pad;
				text.y = y;
				box.addChild(text);
				y += text.height + GAP * a_scale;
			}

			if (a_hasInput) {
				// Measured with text in it: an empty field is no height.
				var typed:TextField = Style.Text("Ag", TEXT_SIZE * a_scale, a_color, a_scale);
				var inputHeight:Number = typed.height;
				typed.text = a_input;
				var inputPad:Number = INPUT_PAD * a_scale;
				typed.x = pad + inputPad;
				typed.y = y + inputPad;
				var frame:Shape = Brackets.Scrolling(inner, inputHeight + inputPad * 2, INPUT_TICK * a_scale, a_color, a_scale, false);
				frame.x = pad;
				frame.y = y;
				box.addChild(frame);
				box.addChild(typed);
				caret = new Shape();
				caret.graphics.beginFill(a_color);
				caret.graphics.drawRect(0, 0, Math.max(1, CARET_WIDTH * a_scale), inputHeight - Style.TEXT_GUTTER * 2);
				caret.graphics.endFill();
				caret.x = Math.round(typed.x + Style.TEXT_GUTTER + typed.textWidth + 1);
				caret.y = typed.y + Style.TEXT_GUTTER;
				box.addChild(caret);
				caretFrom = getTimer();  // each key typed draws this again, so it shows as it's typed
				addEventListener(Event.ENTER_FRAME, Blink);
				addEventListener(Event.REMOVED_FROM_STAGE, Gone);
				y += inputHeight + inputPad * 2 + GAP * a_scale;
			}

			// A little over: exactly this many rows mustn't round down to one too few, and scroll.
			var shown:Number = a_layout.barHeight + a_layout.pitch * Math.max(0, a_labels.length - 1) + 1;
			rows.Build(a_labels, new Array(a_labels.length), { pitch: a_layout.pitch, barHeight: a_layout.barHeight, barInset: pad * 0.5, textInset: pad,
				width: width, height: shown }, a_color, a_scale);
			rows.y = y;
			box.addChild(rows);
			y += shown + pad;

			// Solid, as the game's own message box is, so the page under it doesn't show through.
			box.graphics.beginFill(Style.BLACK, 1);
			box.graphics.drawRect(0, 0, width, y);
			box.graphics.endFill();
			box.addChildAt(Brackets.Panel(width, y, TICK * a_scale, a_color, a_scale), 0);
			box.x = Math.round((a_panelWidth - width) / 2);
			box.y = Math.round(Math.max(0, (a_panelHeight - y) / 2));
			addChild(box);
		}

		private function Blink(a_e:Event):void
		{
			caret.visible = int((getTimer() - caretFrom) / CARET_EVERY) % 2 == 0;
		}

		private function Gone(a_e:Event):void
		{
			removeEventListener(Event.ENTER_FRAME, Blink);
			removeEventListener(Event.REMOVED_FROM_STAGE, Gone);
		}
	}
}
