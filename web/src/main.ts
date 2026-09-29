import { mount } from 'svelte';
import App from './App.svelte';
import './styles/tokens.css';
import './styles/app.css';
import { installWebSdrCompatibility } from './compat/websdr';
import { loadFontWhenTheLinkAllows } from './state/link-room';

const root = document.getElementById('app');
if (root) mount(App, { target: root });
installWebSdrCompatibility();
loadFontWhenTheLinkAllows();
