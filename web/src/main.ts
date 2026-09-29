import { mount } from 'svelte';
import App from './App.svelte';
import '@fontsource-variable/inter/opsz.css';
import './styles/tokens.css';
import './styles/app.css';
import { installWebSdrCompatibility } from './compat/websdr';

const root = document.getElementById('app');
if (root) mount(App, { target: root });
installWebSdrCompatibility();
