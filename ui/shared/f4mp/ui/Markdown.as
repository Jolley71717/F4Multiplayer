package f4mp.ui
{
	import flash.text.TextField;
	import flash.text.TextFormat;

	// As much markdown as the release notes use, for a text field: # headings, - or * lists, paragraphs (a source's single line breaks
	// are only wrapping), and a short line ending in a colon as a label. Inline, `code`, **bold** and [links](url) become their plain text.
	public final class Markdown
	{
		private static const LABEL_LONGEST:int = 40;
		private static const HEADING_SHARE:Number = 1.25;  // of the text's size
		private static const BULLET:String = "- ";

		private var text:String = "";
		private var headings:Array = [];  // [start, end] of each in text
		private var labels:Array = [];
		private var bullets:Array = [];

		public function Markdown(a_source:String)
		{
			var kind:int = 0;  // what's being gathered: 0 a paragraph, 1 a bullet
			var gathered:String = "";
			var lines:Array = a_source.split("\n");
			for (var i:int = 0; i < lines.length; ++i) {
				var line:String = Trim(String(lines[i]));
				if (line == "") {
					Flush(kind, gathered);
					gathered = "";
				} else if (line.charAt(0) == "#") {
					Flush(kind, gathered);
					gathered = "";
					Heading(line);
				} else if (line.indexOf("- ") == 0 || line.indexOf("* ") == 0) {
					Flush(kind, gathered);
					kind = 1;
					gathered = line.substr(2);
				} else {
					if (gathered == "") {
						kind = 0;
					}
					gathered += (gathered == "" ? "" : " ") + line;
				}
			}
			Flush(kind, gathered);
		}

		// Into a text field made with Style.Wrapped, at the size it was made with.
		public function ApplyTo(a_field:TextField, a_size:Number):void
		{
			a_field.text = text;
			var i:int;
			for (i = 0; i < headings.length; ++i) {
				a_field.setTextFormat(new TextFormat(Style.BOLD_FONT, a_size * HEADING_SHARE), headings[i][0], headings[i][1]);
			}
			for (i = 0; i < labels.length; ++i) {
				a_field.setTextFormat(new TextFormat(Style.BOLD_FONT), labels[i][0], labels[i][1]);
			}
			// Lines after a bullet's first start under its text.
			var hang:Number = a_size * 0.6;
			var format:TextFormat = new TextFormat();
			format.leftMargin = hang;
			format.indent = -hang;
			for (i = 0; i < bullets.length; ++i) {
				a_field.setTextFormat(format, bullets[i][0], bullets[i][1]);
			}
		}

		private function Heading(a_line:String):void
		{
			var at:int = 0;
			while (a_line.charAt(at) == "#") {
				++at;
			}
			if (text != "") {
				text += "\n";  // a line of space over it
			}
			Add(headings, Inline(Trim(a_line.substr(at))));
		}

		private function Flush(a_kind:int, a_gathered:String):void
		{
			if (a_gathered == "") {
				return;
			}
			var plain:String = Inline(a_gathered);
			if (a_kind == 1) {
				Add(bullets, BULLET + plain);
			} else if (plain.length <= LABEL_LONGEST && plain.charAt(plain.length - 1) == ":") {
				if (text != "" && text.charAt(text.length - 2) != "\n") {
					text += "\n";  // a line of space over it, after a list
				}
				Add(labels, plain);
			} else {
				text += plain + "\n\n";
			}
		}

		private function Add(a_to:Array, a_line:String):void
		{
			a_to.push([ text.length, text.length + a_line.length ]);
			text += a_line + "\n";
		}

		private static function Inline(a_text:String):String
		{
			var out:String = a_text.split("`").join("").split("**").join("");
			// [text](url): the text.
			var open:int = out.indexOf("[");
			while (open >= 0) {
				var middle:int = out.indexOf("](", open);
				var close:int = middle < 0 ? -1 : out.indexOf(")", middle);
				if (close < 0) {
					break;
				}
				out = out.substring(0, open) + out.substring(open + 1, middle) + out.substring(close + 1);
				open = out.indexOf("[", open);
			}
			return out;
		}

		private static function Trim(a_text:String):String
		{
			var start:int = 0;
			var end:int = a_text.length;
			while (start < end && a_text.charCodeAt(start) <= 0x20) {
				++start;
			}
			while (end > start && a_text.charCodeAt(end - 1) <= 0x20) {
				--end;
			}
			return a_text.substring(start, end);
		}
	}
}
