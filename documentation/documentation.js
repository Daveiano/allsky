// This file resides on the Allsky Github page with a copy on a person's Pi.

// "onPi" and branch are updated during installation.
var onPi = false;
var branch = "dev";

var preURL;			// What gets prepended to the desired URL.
var attributeName;

if (onPi) {
	// "/documentation" is a web alias to ~/allsky/documentation
	preURL = "/documentation/";
	attributeName = "Pi";
} else {
	// To make the URLs shorter, they are only relative to the "documentation" directory.
	var dir = "https://github.com/thomasjacquin/allsky/tree/" + branch + "/documentation/";
	preURL = "https://htmlpreview.github.io/?" + dir;
	attributeName = "href";
}


// Go to the specified URL, either on GitHub or the Pi.
function u(id) {
	var me = document.querySelector("#" + id);
	if (! me) return false;

	var url = me.getAttribute(attributeName);
	if (! url) return false;
	
	window.location.href = preURL + url;

	return false;	// shouldn't get here...
}


// Include a file (e.g., header, footer, sidebar) in a page using Javascript.
function includeHTML() {
	var z, i, elmnt, file, xhttp;
	/* Loop through a collection of all HTML elements: */
	z = document.getElementsByTagName("*");
	for (i = 0; i < z.length; i++) {
		elmnt = z[i];
		/*search for elements with a certain atrribute:*/
		file = elmnt.getAttribute("w3-include-html");
		if (file) {
			/* Make an HTTP request using the attribute value as the file name: */
			xhttp = new XMLHttpRequest();
			xhttp.onreadystatechange = function() {
				if (this.readyState == 4) {
					if (this.status == 200) {elmnt.innerHTML = this.responseText;}
					if (this.status == 404) {elmnt.innerHTML = "Page not found.";}
					/* Remove the attribute, and call this function once more: */
					elmnt.removeAttribute("w3-include-html");
					includeHTML();
				}
			}
			xhttp.open("GET", preURL + file, true);
			xhttp.send();
			/* Exit the function: */
			return;
		}
	}
}
