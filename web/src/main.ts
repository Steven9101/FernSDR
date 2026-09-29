import { mount } from 'svelte';
import App from './App.svelte';
// The weight axis only, 48 KB for Latin: the optical-size axis made the
// file half as large again, 73 KB, for a difference the page's few sizes
// hardly show, and every first visit on a slow link paid for it.
import '@fontsource-variable/inter/wght.css';
import './styles/tokens.css';
import './styles/app.css';
import { installWebSdrCompatibility } from './compat/websdr';

const root = document.getElementById('app');
if (root) mount(App, { target: root });
installWebSdrCompatibility();
