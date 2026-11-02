package f4mp.ui
{
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.MouseEvent;
	import flash.text.TextField;

	// A list of entries as the game's second column shows them: one a row, the chosen one a bar in the color across the
	// panel with its text black. Up and Down wrap; the mouse chooses by hovering and presses by clicking. What a press or
	// a move does is the owner's (a_onPress(index), a_onMove()).
	public class EntryList extends Sprite
	{
		private var rows:Array = [];
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
		}

		public function get length():int
		{
			return rows.length;
		}

		// a_labels: the entries' text fields (Style.Text), already measured by the owner for the panel's width. a_layout:
		// pitch, barHeight, barInset, textInset, width, all scaled.
		public function Build(a_labels:Array, a_layout:Object, a_color:uint, a_scale:Number):void
		{
			Style.Clear(this);
			rows = [];
			color = a_color;
			scale = a_scale;
			for (var i:int = 0; i < a_labels.length; ++i) {
				var row:Sprite = Row(a_labels[i], i, a_layout);
				row.y = a_layout.pitch * i;
				addChild(row);
				rows.push(row);
			}
			selected = -1;
			Select(rows.length > 0 ? 0 : -1, false);
		}

		public function Move(a_by:int):void
		{
			if (rows.length > 0) {
				Select((selected + a_by + rows.length) % rows.length, true);
			}
		}

		public function PressSelected():void
		{
			Press(selected);
		}

		private function Row(a_text:TextField, a_index:int, a_layout:Object):Sprite
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

			row.buttonMode = true;
			row.addEventListener(MouseEvent.ROLL_OVER, function(a_e:MouseEvent):void { Select(a_index, true); });
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
				if (a_moved) {
					onMove();
				}
			}
		}

		// The chosen entry: the bar in the color, its text black with no shadow.
		private function Paint(a_row:Sprite, a_on:Boolean):void
		{
			a_row.getChildByName("bar").visible = a_on;
			var text:TextField = a_row.getChildByName("text") as TextField;
			text.textColor = a_on ? Style.BLACK : color;
			text.filters = a_on ? [] : [ Style.Shadow(scale) ];
		}

		private function Press(a_index:int):void
		{
			if (a_index >= 0 && a_index < rows.length) {
				onPress(a_index);
			}
		}
	}
}
