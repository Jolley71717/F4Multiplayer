package f4mp.ui
{
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.MouseEvent;
	import flash.text.TextField;

	// A list of entries as the game's second column shows them: one a row, the chosen one a bar in the color across the
	// panel with its text black. Up and Down stop at the ends; the mouse chooses by hovering and presses by clicking. What a
	// press or a move does is the owner's (a_onPress(index), a_onMove()). Longer than the height given, it scrolls to the
	// chosen one, a whole row at a time, with the game's double chevrons where there is more above or below.
	public class EntryList extends Sprite
	{
		public static const CHEVRON_SLOT:Number = 0.42;  // the chevrons' room, of a row's pitch: the owner leaves this above the list
		private static const VALUE_AT:Number = 0.75;  // the middle of the values' column, of the row's width
		// The game's own (the ScrollUp symbol of its lists, MainMenu.swf), in its units: two filled chevrons, pointing up.
		private static const CHEVRON_WIDE:Number = 13.5;
		private static const CHEVRON_TALL:Number = 11.55;
		private static const CHEVRON_ARM:Number = 1.9;     // the gap between a chevron's ends, in from each side
		private static const CHEVRON_RISE:Number = 6;      // a chevron's height at its outer edge
		private static const CHEVRON_INNER:Number = 1.7;   // its thickness at the tip
		private static const CHEVRON_APART:Number = 5.55;

		private var rows:Array = [];
		private var scroller:Sprite = new Sprite();
		private var chrome:Sprite = new Sprite();  // the chevrons, when it scrolls
		private var moreAbove:Sprite = null;
		private var moreBelow:Sprite = null;
		private var first:int = 0;     // the top row showing
		private var shownRows:int = 1;
		private var pitch:Number = 0;
		private var barHeight:Number = 0;
		private var shownHeight:Number = 0;
		private var selected:int = -1;
		private var color:uint;
		private var scale:Number;
		private var onPress:Function;
		private var onMove:Function;

		public function EntryList(a_onPress:Function, a_onMove:Function)
		{
			super();
			onPress = a_onPress;
			onMove = a_onMove;
			addChild(scroller);
			addChild(chrome);
		}

		public function get selectedIndex():int
		{
			return selected;
		}

		// Whether some rows don't fit.
		public function get scrolls():Boolean
		{
			return rows.length > shownRows;
		}

		public function get length():int
		{
			return rows.length;
		}

		// a_labels: the entries' text fields (Style.Text), already measured by the owner for the panel's width. a_values: for
		// each, the text field of what it's set to, in a column toward the row's right (null: none). a_layout:
		// pitch, barHeight, barInset, textInset, width and height (of the part shown), all scaled.
		private var builtMouseX:Number = 0;
		private var builtMouseY:Number = 0;

		public function Build(a_labels:Array, a_values:Array, a_layout:Object, a_color:uint, a_scale:Number):void
		{
			Style.Clear(scroller);
			Style.Clear(chrome);
			moreAbove = null;
			moreBelow = null;
			first = 0;
			rows = [];
			pitch = a_layout.pitch;
			barHeight = a_layout.barHeight;
			shownHeight = a_layout.height;
			// Room for the chevrons: above the first row is its owner's gap, below the last is taken from the height.
			var slot:Number = pitch * CHEVRON_SLOT;
			shownRows = Math.max(1, Math.floor((shownHeight - barHeight) / pitch) + 1);
			if (a_labels.length > shownRows) {
				shownRows = Math.max(1, Math.floor((shownHeight - slot - barHeight) / pitch) + 1);
			}
			color = a_color;
			scale = a_scale;
			for (var i:int = 0; i < a_labels.length; ++i) {
				var row:Sprite = Row(a_labels[i], a_values[i], i, a_layout);
				row.y = a_layout.pitch * i;
				scroller.addChild(row);
				rows.push(row);
			}
			selected = -1;
			builtMouseX = mouseX;
			builtMouseY = mouseY;
			if (scrolls) {
				moreAbove = Chevrons(true, -1, a_color, a_scale);
				moreAbove.x = a_layout.textInset;
				moreAbove.y = -slot + (slot - moreAbove.height) / 2;
				chrome.addChild(moreAbove);
				moreBelow = Chevrons(false, 1, a_color, a_scale);
				moreBelow.x = a_layout.textInset;
				moreBelow.y = pitch * (shownRows - 1) + barHeight + (slot - moreBelow.height) / 2;
				chrome.addChild(moreBelow);
			}
			Select(rows.length > 0 ? 0 : -1, false);
			Place();
		}

		// After Build: the row chosen and the top row shown, as they were on the page before.
		public function Restore(a_selected:int, a_first:int):void
		{
			if (rows.length > 0) {
				first = Math.max(0, Math.min(a_first, rows.length - shownRows));
				Select(Math.max(0, Math.min(a_selected, rows.length - 1)), false);
				Place();
			}
		}

		public function get firstRow():int
		{
			return first;
		}

		public function Move(a_by:int):void
		{
			if (rows.length > 0) {
				Select(Math.max(0, Math.min(rows.length - 1, selected + a_by)), true);
			}
		}

		public function PressSelected():void
		{
			Press(selected);
		}

		private function Row(a_text:TextField, a_value:TextField, a_index:int, a_layout:Object):Sprite
		{
			var row:Sprite = new Sprite();
			var bar:Shape = new Shape();
			bar.name = "bar";
			bar.graphics.beginFill(color, 1);
			bar.graphics.drawRect(a_layout.barInset, 0, a_layout.width - a_layout.barInset * 2, a_layout.barHeight);
			bar.graphics.endFill();
			bar.visible = false;
			row.addChild(bar);

			a_text.name = "text";
			a_text.x = a_layout.textInset;
			a_text.y = (a_layout.barHeight - a_text.height) / 2;
			row.addChild(a_text);
			if (a_value != null) {
				a_value.name = "value";
				var end:Number = a_layout.width - a_layout.barInset - a_layout.textInset * 0.5;
				a_value.x = Math.min(end - a_value.width, a_layout.width * VALUE_AT - a_value.width / 2);
				a_value.y = a_text.y;
				row.addChild(a_value);
			}

			row.buttonMode = true;
			row.addEventListener(MouseEvent.ROLL_OVER, function(a_e:MouseEvent):void {
				// Only a mouse that moved: a row built under a still one rolls over too, and a redraw would take the choice from the keys.
				if (mouseX != builtMouseX || mouseY != builtMouseY) {
					Select(a_index, true);
				}
			});
			row.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Press(a_index); });
			return row;
		}

		private function Select(a_index:int, a_moved:Boolean):void
		{
			if (a_index == selected) {
				return;
			}
			if (selected >= 0 && selected < rows.length) {
				Paint(rows[selected], false);
			}
			selected = a_index;
			if (selected >= 0 && selected < rows.length) {
				Paint(rows[selected], true);
				if (selected < first) {
					first = selected;
				} else if (selected >= first + shownRows) {
					first = selected - shownRows + 1;
				}
				Place();
				if (a_moved) {
					onMove();
				}
			}
		}

		// Two chevrons, as the game's lists show. A click moves the rows on by one, and the chosen one if it would go out of sight.
		private function Chevrons(a_up:Boolean, a_by:int, a_color:uint, a_scale:Number):Sprite
		{
			var chevrons:Sprite = new Sprite();
			var wide:Number = CHEVRON_WIDE * a_scale;
			var tall:Number = CHEVRON_TALL * a_scale;
			chevrons.graphics.beginFill(Style.BLACK, 0.001);
			chevrons.graphics.drawRect(0, 0, wide, tall);
			chevrons.graphics.endFill();
			var lines:Shape = new Shape();
			// From the top of the pair down, flipped for the lower one, as the game flips the symbol.
			var y:Function = function(a_y:Number):Number { return (a_up ? a_y : CHEVRON_TALL - a_y) * a_scale; };
			lines.graphics.beginFill(a_color);
			for (var i:int = 0; i < 2; ++i) {
				var from:Number = CHEVRON_APART * i;
				lines.graphics.moveTo(0, y(from + CHEVRON_RISE));
				lines.graphics.lineTo(wide / 2, y(from));
				lines.graphics.lineTo(wide, y(from + CHEVRON_RISE));
				lines.graphics.lineTo(wide - CHEVRON_ARM * a_scale, y(from + CHEVRON_RISE));
				lines.graphics.lineTo(wide / 2, y(from + CHEVRON_INNER));
				lines.graphics.lineTo(CHEVRON_ARM * a_scale, y(from + CHEVRON_RISE));
				lines.graphics.lineTo(0, y(from + CHEVRON_RISE));
			}
			lines.graphics.endFill();
			lines.filters = [ Style.Shadow(a_scale) ];
			chevrons.addChild(lines);
			chevrons.buttonMode = true;
			chevrons.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void {
				first = Math.max(0, Math.min(rows.length - shownRows, first + a_by));
				Place();
				if (selected < first) {
					Select(first, true);
				} else if (selected >= first + shownRows) {
					Select(first + shownRows - 1, true);
				}
			});
			return chevrons;
		}

		// Only the rows that fit whole show. A mask here failed on the first page drawn (no clipping, and a block of its
		// own), and clipped only after the first input.
		private function Place():void
		{
			scroller.y = -first * pitch;
			for (var i:int = 0; i < rows.length; ++i) {
				rows[i].visible = i >= first && i < first + shownRows;
			}
			if (moreAbove != null) {
				moreAbove.visible = first > 0;
				moreBelow.visible = first + shownRows < rows.length;
			}
		}

		// The chosen entry: the bar in the color, its text black with no shadow.
		private function Paint(a_row:Sprite, a_on:Boolean):void
		{
			a_row.getChildByName("bar").visible = a_on;
			var text:TextField = a_row.getChildByName("text") as TextField;
			text.textColor = a_on ? Style.BLACK : color;
			text.filters = a_on ? [] : [ Style.Shadow(scale) ];
			var value:TextField = a_row.getChildByName("value") as TextField;
			if (value != null) {
				value.textColor = text.textColor;
				value.filters = text.filters;
			}
		}

		private function Press(a_index:int):void
		{
			if (a_index >= 0 && a_index < rows.length) {
				onPress(a_index);
			}
		}
	}
}
