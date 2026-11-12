/* Draw the Mermaid diagrams on the site the way github.com draws them.
 *
 * Why not Material's own Mermaid support: it repaints every node label with its theme colour
 * (".label { color: var(--md-mermaid-label-fg-color) }"), so the colours a diagram chooses with
 * classDef - white text on the house palette, black on amber - never show. The diagrams here
 * follow the RetroCore Labs Mermaid colour standard and must look the same on the site as on
 * github.com. So the Mermaid fences get the class "mermaid-gh" (mkdocs.yml), which Material
 * leaves alone, and this script draws them with Mermaid's own themes: "default" in the light
 * scheme, "dark" in the dark one, drawn again when the reader switches.
 */
(function () {
    var blocks = [];

    function theme() {
        return document.body.getAttribute('data-md-color-scheme') === 'slate' ? 'dark' : 'default';
    }

    async function draw() {
        if (!window.mermaid) {
            return;
        }
        window.mermaid.initialize({startOnLoad: false, theme: theme(), securityLevel: 'strict'});
        for (var i = 0; i < blocks.length; i++) {
            try {
                var r = await window.mermaid.render('mermaid-gh-' + i + '-' + Date.now(),
                                                   blocks[i].src);
                blocks[i].el.innerHTML = r.svg;
            } catch (e) {
                /* Show the source rather than nothing, and say why. */
                blocks[i].el.textContent = 'This diagram could not be drawn: ' + e.message;
            }
        }
    }

    function start() {
        var pres = document.querySelectorAll('pre.mermaid-gh');
        for (var i = 0; i < pres.length; i++) {
            var d = document.createElement('div');
            d.className = 'mermaid-gh-drawn';
            blocks.push({el: d, src: pres[i].textContent});
            pres[i].replaceWith(d);
        }
        if (blocks.length === 0) {
            return;
        }
        draw();
        new MutationObserver(draw).observe(document.body,
                                           {attributes: true, attributeFilter: ['data-md-color-scheme']});
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', start);
    } else {
        start();
    }
})();
